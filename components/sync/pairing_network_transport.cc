// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/pairing_network_transport.h"

#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace compass::sync {
namespace {

constexpr char kPairTokenHeader[] = "X-Compass-Pair-Token";
constexpr size_t kMaxReplySize = 64 * 1024;
// Longer than the server's 25 s long poll.
constexpr base::TimeDelta kRequestTimeout = base::Seconds(40);

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_sync_pairing", R"(
        semantics {
          sender: "Compass sync pairing"
          description:
            "Adds a device to the user's sync chain. Relays end-to-end "
            "encrypted pairing messages between two of the user's devices "
            "through the sync server the user chose."
          trigger: "The user adds a device, or joins one, in Compass's sync "
                   "page."
          data: "SPAKE2 messages and data encrypted with the pairing key; "
                "the chain's signed token on the device already syncing."
          destination: OTHER
          destination_other: "The self-hosted sync server the user set."
        }
        policy {
          cookies_allowed: NO
          setting: "Only runs while the user pairs two devices."
          policy_exception_justification: "Not implemented."
        })");

const char* RoleName(PairingRole role) {
  return role == PairingRole::kA ? "a" : "b";
}

}  // namespace

NetworkPairingTransport::NetworkPairingTransport(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    const GURL& server_url,
    TokenGetter token_getter)
    : url_loader_factory_(std::move(url_loader_factory)),
      server_url_(server_url),
      token_getter_(std::move(token_getter)) {}

NetworkPairingTransport::~NetworkPairingTransport() = default;

// static
GURL NetworkPairingTransport::MakePairUrl(const GURL& server_url,
                                          std::string_view suffix) {
  std::string path = server_url.GetPath();
  while (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  path += "/compass/pair";
  if (!suffix.empty()) {
    base::StrAppend(&path, {"/", suffix});
  }
  GURL::Replacements replacements;
  replacements.SetPathStr(path);
  replacements.ClearQuery();
  replacements.ClearRef();
  return server_url.ReplaceComponents(replacements);
}

void NetworkPairingTransport::Open(OpenCallback callback) {
  Request("POST", MakePairUrl(server_url_, ""), PairingRole::kA, "{}",
          base::BindOnce(
              [](OpenCallback callback, int status,
                 std::optional<base::DictValue> reply) {
                const std::optional<int> slot =
                    reply ? reply->FindInt("slot") : std::nullopt;
                const std::optional<int> expires_in =
                    reply ? reply->FindInt("expires_in") : std::nullopt;
                if (status / 100 == 2 && (!slot || !expires_in)) {
                  status = 0;
                }
                std::move(callback).Run(status, slot.value_or(0),
                                        base::Seconds(expires_in.value_or(0)));
              },
              std::move(callback)));
}

void NetworkPairingTransport::Join(int slot, StatusCallback callback) {
  Request("POST",
          MakePairUrl(server_url_,
                      base::StrCat({base::NumberToString(slot), "/join"})),
          std::nullopt, "{}",
          base::BindOnce(
              [](base::WeakPtr<NetworkPairingTransport> self,
                 StatusCallback callback, int status,
                 std::optional<base::DictValue> reply) {
                const std::string* token =
                    reply ? reply->FindString("b_token") : nullptr;
                if (status / 100 == 2) {
                  if (!token || token->empty()) {
                    status = 0;
                  } else if (self) {
                    self->b_token_ = *token;
                  }
                }
                std::move(callback).Run(status);
              },
              weak_factory_.GetWeakPtr(), std::move(callback)));
}

void NetworkPairingTransport::Send(int slot,
                                   PairingRole from,
                                   std::vector<uint8_t> data,
                                   StatusCallback callback) {
  base::DictValue body;
  body.Set("from", RoleName(from));
  body.Set("data", base::Base64Encode(data));
  std::optional<std::string> json =
      base::WriteJson(base::Value(std::move(body)));
  Request("POST",
          MakePairUrl(server_url_,
                      base::StrCat({base::NumberToString(slot), "/msg"})),
          from, std::move(json),
          base::BindOnce(
              [](StatusCallback callback, int status,
                 std::optional<base::DictValue> reply) {
                std::move(callback).Run(status);
              },
              std::move(callback)));
}

void NetworkPairingTransport::Receive(int slot,
                                      PairingRole reader,
                                      int after,
                                      ReceiveCallback callback) {
  GURL url = MakePairUrl(server_url_,
                         base::StrCat({base::NumberToString(slot), "/msg"}));
  const std::string query = base::StrCat(
      {"for=", RoleName(reader), "&after=", base::NumberToString(after)});
  GURL::Replacements replacements;
  replacements.SetQueryStr(query);
  url = url.ReplaceComponents(replacements);
  Request("GET", url, reader, std::nullopt,
          base::BindOnce(
              [](ReceiveCallback callback, int status,
                 std::optional<base::DictValue> reply) {
                std::vector<PairingMessage> messages;
                if (status / 100 == 2) {
                  const base::ListValue* list =
                      reply ? reply->FindList("messages") : nullptr;
                  if (!list) {
                    status = 0;
                  } else {
                    for (const base::Value& item : *list) {
                      const base::DictValue* dict = item.GetIfDict();
                      const std::optional<int> seq =
                          dict ? dict->FindInt("seq") : std::nullopt;
                      const std::string* data =
                          dict ? dict->FindString("data") : nullptr;
                      std::optional<std::vector<uint8_t>> bytes =
                          data ? base::Base64Decode(*data) : std::nullopt;
                      if (!seq || !bytes) {
                        status = 0;
                        messages.clear();
                        break;
                      }
                      messages.push_back({*seq, std::move(*bytes)});
                    }
                  }
                }
                std::move(callback).Run(status, std::move(messages));
              },
              std::move(callback)));
}

void NetworkPairingTransport::Close(int slot, PairingRole by) {
  std::unique_ptr<network::SimpleURLLoader> loader =
      MakeLoader("DELETE", MakePairUrl(server_url_, base::NumberToString(slot)),
                 by, std::nullopt);
  network::SimpleURLLoader* raw = loader.get();
  // Owned by its own callback, so it outlives |this|.
  raw->DownloadHeadersOnly(
      url_loader_factory_.get(),
      base::BindOnce([](std::unique_ptr<network::SimpleURLLoader>,
                        scoped_refptr<net::HttpResponseHeaders>) {},
                     std::move(loader)));
}

std::unique_ptr<network::SimpleURLLoader> NetworkPairingTransport::MakeLoader(
    const std::string& method,
    const GURL& url,
    std::optional<PairingRole> auth,
    std::optional<std::string> body) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->method = method;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  if (auth == PairingRole::kA) {
    request->headers.SetHeader(net::HttpRequestHeaders::kAuthorization,
                               base::StrCat({"Bearer ", token_getter_.Run()}));
  } else if (auth == PairingRole::kB) {
    request->headers.SetHeader(kPairTokenHeader, b_token_);
  }
  std::unique_ptr<network::SimpleURLLoader> loader =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  if (body) {
    loader->AttachStringForUpload(std::move(*body), "application/json");
  }
  loader->SetTimeoutDuration(kRequestTimeout);
  loader->SetAllowHttpErrorResults(true);
  return loader;
}

void NetworkPairingTransport::Request(const std::string& method,
                                      const GURL& url,
                                      std::optional<PairingRole> auth,
                                      std::optional<std::string> body,
                                      ResponseCallback callback) {
  loaders_.push_front(MakeLoader(method, url, auth, std::move(body)));
  auto it = loaders_.begin();
  (*it)->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&NetworkPairingTransport::OnResponse,
                     weak_factory_.GetWeakPtr(), it, std::move(callback)),
      kMaxReplySize);
}

void NetworkPairingTransport::OnResponse(Loaders::iterator it,
                                         ResponseCallback callback,
                                         std::optional<std::string> body) {
  std::unique_ptr<network::SimpleURLLoader> loader = std::move(*it);
  loaders_.erase(it);
  int status = 0;
  if (const network::mojom::URLResponseHead* head = loader->ResponseInfo();
      head && head->headers) {
    status = head->headers->response_code();
  }
  std::optional<base::DictValue> reply;
  if (body && !body->empty()) {
    reply = base::JSONReader::ReadDict(*body, base::JSON_PARSE_RFC);
  }
  std::move(callback).Run(status, std::move(reply));
}

}  // namespace compass::sync
