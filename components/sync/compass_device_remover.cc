// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/compass_device_remover.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "compass/components/sync/device_state.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace compass::sync {
namespace {

constexpr size_t kMaxReplySize = 4 * 1024;
constexpr base::TimeDelta kRequestTimeout = base::Seconds(30);
// Set on a 401 when the server wants its sign-in first.
constexpr char kLoginHeader[] = "X-Compass-Login";

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_sync_remove_device", R"(
        semantics {
          sender: "Compass sync"
          description:
            "Removes one of the user's other devices from the user's sync "
            "chain on the user's sync server: the server deletes the "
            "device's entry and stops accepting its requests."
          trigger: "The user chooses Remove for a device on the sync page "
                   "and confirms."
          data: "The chain's signed token, the removed device's sync ID and "
                "this device's sync ID."
          destination: OTHER
          destination_other: "The self-hosted sync server the user set."
        }
        policy {
          cookies_allowed: NO
          setting: "Only while the user syncs through a Compass sync server."
          policy_exception_justification: "Not implemented."
        })");

}  // namespace

bool IsDeviceRemovedResponse(const net::HttpResponseHeaders* headers) {
  if (!headers) {
    return false;
  }
  return IsDeviceRemovedReply(
      headers->response_code(),
      headers->GetNormalizedHeader(kDeviceStateHeader).value_or(std::string()));
}

GURL MakeDeviceUrl(const GURL& server_url,
                   std::string_view cache_guid,
                   std::string_view client_id) {
  std::string path = server_url.GetPath();
  while (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  // Cache GUIDs are base64: '/' and '+' must not split or change the path.
  base::StrAppend(&path, {"/compass/devices/",
                          base::EscapeAllExceptUnreserved(cache_guid)});
  const std::string query = base::StrCat(
      {"client_id=",
       base::EscapeQueryParamValue(client_id, /*use_plus=*/false)});
  GURL::Replacements replacements;
  replacements.SetPathStr(path);
  replacements.SetQueryStr(query);
  replacements.ClearRef();
  return server_url.ReplaceComponents(replacements);
}

DeviceRemover::DeviceRemover(
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    const GURL& server_url,
    TokenGetter token_getter)
    : loader_factory_(std::move(loader_factory)),
      server_url_(server_url),
      token_getter_(std::move(token_getter)) {}

DeviceRemover::~DeviceRemover() = default;

// static
DeviceRemover::Result DeviceRemover::ResultOf(
    const net::HttpResponseHeaders* headers) {
  if (!headers) {
    return Result::kFailed;
  }
  const int status = headers->response_code();
  if (status >= 200 && status < 300) {
    return Result::kRemoved;
  }
  if (IsDeviceRemovedResponse(headers)) {
    return Result::kDeviceRemoved;
  }
  switch (status) {
    case 401:
      return headers->HasHeader(kLoginHeader) ? Result::kLoginRequired
                                              : Result::kUnauthorized;
    case 404:
    case 405:
      return Result::kUnsupported;
    case 409:
      return Result::kThisDevice;
    default:
      return Result::kFailed;
  }
}

void DeviceRemover::Remove(const std::string& cache_guid,
                           const std::string& client_id,
                           Callback callback) {
  const std::optional<std::string> token = token_getter_.Run();
  if (!token || cache_guid.empty() || client_id.empty()) {
    std::move(callback).Run(Result::kFailed);
    return;
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = MakeDeviceUrl(server_url_, cache_guid, client_id);
  request->method = "DELETE";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader(net::HttpRequestHeaders::kAuthorization,
                             base::StrCat({"Bearer ", *token}));
  loaders_.push_front(
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation));
  Loaders::iterator loader = loaders_.begin();
  (*loader)->SetTimeoutDuration(kRequestTimeout);
  // The status and headers say how it went, error or not.
  (*loader)->SetAllowHttpErrorResults(true);
  (*loader)->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&DeviceRemover::OnResponse, weak_factory_.GetWeakPtr(),
                     loader, std::move(callback)),
      kMaxReplySize);
}

void DeviceRemover::OnResponse(Loaders::iterator loader,
                               Callback callback,
                               std::optional<std::string> body) {
  const network::mojom::URLResponseHead* head = (*loader)->ResponseInfo();
  const Result result = ResultOf(head ? head->headers.get() : nullptr);
  loaders_.erase(loader);
  std::move(callback).Run(result);
}

}  // namespace compass::sync
