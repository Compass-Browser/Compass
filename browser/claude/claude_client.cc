// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_client.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "net/base/net_errors.h"
#include "net/base/url_util.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace compass::claude {

namespace {

constexpr size_t kMaxModelsReplySize = 1024 * 1024;
constexpr size_t kMaxErrorBodySize = 64 * 1024;
constexpr base::TimeDelta kModelsTimeout = base::Seconds(30);

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_claude", R"(
        semantics {
          sender: "Compass Claude"
          description:
            "Sends the user's messages to Claude, with the page content and "
            "tool results the chat gathers, and lists the models the user's "
            "key can use."
          trigger: "The user sends a message in the Claude chat, tests the "
                   "connection, or opens the model list in settings."
          data: "The conversation, page content Claude reads, and the API "
                "key the user entered."
          destination: OTHER
          destination_other: "The Claude API (api.anthropic.com) or the "
                             "endpoint the user set in settings."
        }
        policy {
          cookies_allowed: NO
          setting: "Only after the user enters an API key in Compass's "
                   "Claude settings, and only when they use the chat."
          policy_exception_justification: "Not implemented."
        })");

int ResponseCode(const network::SimpleURLLoader& loader) {
  const network::mojom::URLResponseHead* head = loader.ResponseInfo();
  return head && head->headers ? head->headers->response_code() : 0;
}

// The message of an "error" event's data.
std::string StreamErrorMessage(std::string_view data) {
  if (std::optional<base::DictValue> dict =
          base::JSONReader::ReadDict(data, base::JSON_PARSE_RFC)) {
    if (const base::DictValue* error = dict->FindDict("error")) {
      if (const std::string* message = error->FindString("message")) {
        return *message;
      }
    }
  }
  return "Claude stopped with an error.";
}

}  // namespace

ClaudeClient::ClaudeClient(
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    Endpoint endpoint)
    : loader_factory_(std::move(loader_factory)),
      endpoint_(std::move(endpoint)) {}

ClaudeClient::~ClaudeClient() = default;

std::unique_ptr<network::SimpleURLLoader> ClaudeClient::MakeLoader(
    const GURL& url,
    std::string_view method,
    const std::vector<std::string>& betas) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->method = std::string(method);
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  AddRequestHeaders(endpoint_, request->headers);
  if (!betas.empty()) {
    request->headers.SetHeader("anthropic-beta", base::JoinString(betas, ","));
  }
  auto loader =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  // Error replies carry the API's message.
  loader->SetAllowHttpErrorResults(true);
  return loader;
}

void ClaudeClient::ListModels(ModelsCallback callback) {
  if (endpoint_.api_key.empty()) {
    std::move(callback).Run(base::unexpected("Enter an API key first."));
    return;
  }
  models_loader_ = MakeLoader(
      net::AppendQueryParameter(EndpointUrl(endpoint_.base_url, "v1/models"),
                                "limit", "100"),
      "GET");
  models_loader_->SetTimeoutDuration(kModelsTimeout);
  models_loader_->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&ClaudeClient::OnModels, weak_factory_.GetWeakPtr(),
                     std::move(callback)),
      kMaxModelsReplySize);
}

void ClaudeClient::OnModels(ModelsCallback callback,
                            std::optional<std::string> body) {
  const int status = ResponseCode(*models_loader_);
  models_loader_.reset();
  if (status != net::HTTP_OK || !body) {
    std::move(callback).Run(
        base::unexpected(DescribeError(status, body.value_or(""))));
    return;
  }
  std::optional<std::vector<ModelInfo>> models = ParseModelList(*body);
  if (!models) {
    std::move(callback).Run(base::unexpected(
        "The server answered, but not with a list of models. Check the "
        "endpoint URL."));
    return;
  }
  std::move(callback).Run(std::move(*models));
}

void ClaudeClient::StreamMessage(base::DictValue request,
                                 std::vector<std::string> betas,
                                 EventCallback on_event,
                                 DoneCallback on_done) {
  Cancel();
  if (endpoint_.api_key.empty()) {
    std::move(on_done).Run("Enter an API key in Compass's Claude settings.");
    return;
  }
  request.Set("stream", true);
  std::optional<std::string> body = base::WriteJson(request);
  if (!body) {
    std::move(on_done).Run("The request couldn't be written.");
    return;
  }
  on_event_ = std::move(on_event);
  on_done_ = std::move(on_done);
  parser_.Reset();
  stream_status_ = 0;
  error_body_.clear();
  saw_message_stop_ = false;
  stream_loader_ = MakeLoader(EndpointUrl(endpoint_.base_url, "v1/messages"),
                              "POST", betas);
  stream_loader_->AttachStringForUpload(*body, "application/json");
  stream_loader_->SetOnResponseStartedCallback(base::BindOnce(
      &ClaudeClient::OnStreamStarted, weak_factory_.GetWeakPtr()));
  stream_loader_->DownloadAsStream(loader_factory_.get(), this);
}

void ClaudeClient::Cancel() {
  stream_loader_.reset();
  on_event_.Reset();
  on_done_.Reset();
}

void ClaudeClient::OnStreamStarted(
    const GURL& final_url,
    const network::mojom::URLResponseHead& head) {
  stream_status_ = head.headers ? head.headers->response_code() : 0;
}

void ClaudeClient::OnDataReceived(std::string_view data,
                                  base::OnceClosure resume) {
  if (stream_status_ != net::HTTP_OK) {
    if (error_body_.size() < kMaxErrorBodySize) {
      error_body_.append(data.substr(
          0, std::min(data.size(), kMaxErrorBodySize - error_body_.size())));
    }
    std::move(resume).Run();
    return;
  }
  base::WeakPtr<ClaudeClient> self = weak_factory_.GetWeakPtr();
  for (const SseParser::Event& event : parser_.Feed(data)) {
    if (event.name == "error") {
      FinishStream(StreamErrorMessage(event.data));
      return;
    }
    if (event.name == "message_stop") {
      saw_message_stop_ = true;
    }
    on_event_.Run(event);
    // The event may have deleted the client, or cancelled the stream.
    if (!self || !stream_loader_) {
      return;
    }
  }
  std::move(resume).Run();
}

void ClaudeClient::OnComplete(bool success) {
  if (stream_status_ != net::HTTP_OK) {
    FinishStream(DescribeError(stream_status_, error_body_));
  } else if (!success) {
    FinishStream(DescribeError(0, ""));
  } else if (!saw_message_stop_) {
    FinishStream("The reply was cut off. Try again.");
  } else {
    FinishStream(std::nullopt);
  }
}

void ClaudeClient::OnRetry(base::OnceClosure start_retry) {
  // No retries are set up.
  std::move(start_retry).Run();
}

void ClaudeClient::FinishStream(std::optional<std::string> error) {
  // The loader is done (or failed); let go of it before telling, since the
  // callback may start another stream or delete the client.
  stream_loader_.reset();
  on_event_.Reset();
  if (on_done_) {
    std::move(on_done_).Run(std::move(error));
  }
}

}  // namespace compass::claude
