// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/compass_sync_quota.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/values.h"
#include "net/http/http_request_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"

namespace compass::sync {
namespace {

constexpr size_t kMaxReplySize = 16 * 1024;
constexpr base::TimeDelta kRequestTimeout = base::Seconds(30);

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_sync_quota", R"(
        semantics {
          sender: "Compass sync"
          description:
            "Asks the user's sync server how close the user's sync chain is "
            "to the server's limits (items, storage, history), so Compass "
            "can say when the server refuses new data."
          trigger: "After sync cycles while sync is on: at most every 30 "
                   "minutes, or every minute while commits fail or the "
                   "server is full."
          data: "The chain's signed token. The answer holds counts only."
          destination: OTHER
          destination_other: "The self-hosted sync server the user set."
        }
        policy {
          cookies_allowed: NO
          setting: "Only while the user syncs through a Compass sync server."
          policy_exception_justification: "Not implemented."
        })");

int64_t FindCount(const base::DictValue& dict, std::string_view key) {
  // Counts may pass int's range (bytes); FindDouble reads ints too.
  const std::optional<double> v = dict.FindDouble(key);
  return v && *v > 0 ? static_cast<int64_t>(*v) : 0;
}

}  // namespace

std::optional<QuotaStatus> ParseQuotaStatus(std::string_view json) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict) {
    return std::nullopt;
  }
  const base::ListValue* full = dict->FindList("full");
  if (!full || !dict->FindDouble("max_entities")) {
    return std::nullopt;
  }
  QuotaStatus status;
  status.entities = FindCount(*dict, "entities");
  status.max_entities = FindCount(*dict, "max_entities");
  status.bytes = FindCount(*dict, "bytes");
  status.max_bytes = FindCount(*dict, "max_bytes");
  status.history = FindCount(*dict, "history");
  status.max_history = FindCount(*dict, "max_history");
  status.history_days = static_cast<int>(FindCount(*dict, "history_days"));
  for (const base::Value& item : *full) {
    const std::string* name = item.GetIfString();
    if (!name) {
      continue;
    }
    if (*name == "entities") {
      status.entities_full = true;
    } else if (*name == "bytes") {
      status.bytes_full = true;
    } else if (*name == "history") {
      status.history_full = true;
    }
  }
  return status;
}

GURL MakeQuotaUrl(const GURL& server_url) {
  std::string path = server_url.GetPath();
  while (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  path += "/compass/quota";
  GURL::Replacements replacements;
  replacements.SetPathStr(path);
  replacements.ClearQuery();
  replacements.ClearRef();
  return server_url.ReplaceComponents(replacements);
}

QuotaFetcher::QuotaFetcher(
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    const GURL& server_url,
    TokenGetter token_getter)
    : loader_factory_(std::move(loader_factory)),
      url_(MakeQuotaUrl(server_url)),
      token_getter_(std::move(token_getter)) {}

QuotaFetcher::~QuotaFetcher() = default;

bool QuotaFetcher::Fetch(Callback callback) {
  if (loader_) {
    return false;
  }
  const std::optional<std::string> token = token_getter_.Run();
  if (!token) {
    std::move(callback).Run(std::nullopt);
    return true;
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url_;
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader(net::HttpRequestHeaders::kAuthorization,
                             base::StrCat({"Bearer ", *token}));
  loader_ =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  loader_->SetTimeoutDuration(kRequestTimeout);
  loader_->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&QuotaFetcher::OnResponse, weak_factory_.GetWeakPtr(),
                     std::move(callback)),
      kMaxReplySize);
  return true;
}

void QuotaFetcher::OnResponse(Callback callback,
                              std::optional<std::string> body) {
  // Without SetAllowHttpErrorResults, an HTTP error gives no body.
  loader_.reset();
  std::move(callback).Run(body ? ParseQuotaStatus(*body) : std::nullopt);
}

}  // namespace compass::sync
