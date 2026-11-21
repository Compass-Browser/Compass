// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/compass_account_linker.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
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

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_sync_link_account", R"(
        semantics {
          sender: "Compass sync"
          description:
            "Links the sync account of one of the user's browser profiles to "
            "the user's sync chain on the user's sync server, so a server "
            "that serves only invited chains lets that profile sync too."
          trigger: "A profile of the sync chain starts syncing, or the server "
                   "refused its account."
          data: "The chain's signed token and the profile account's signed "
                "token. No browsing data."
          destination: OTHER
          destination_other: "The self-hosted sync server the user set."
        }
        policy {
          cookies_allowed: NO
          setting: "Only while the user syncs through a Compass sync server."
          policy_exception_justification: "Not implemented."
        })");

}  // namespace

GURL MakeAccountLinkUrl(const GURL& server_url) {
  std::string path = server_url.GetPath();
  while (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  path += "/compass/account/link";
  GURL::Replacements replacements;
  replacements.SetPathStr(path);
  replacements.ClearQuery();
  replacements.ClearRef();
  return server_url.ReplaceComponents(replacements);
}

AccountLinker::AccountLinker(
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    const GURL& server_url)
    : loader_factory_(std::move(loader_factory)), server_url_(server_url) {}

AccountLinker::~AccountLinker() = default;

// static
AccountLinker::Result AccountLinker::ResultOf(int status) {
  if ((status >= 200 && status < 300) || status == 404 || status == 405) {
    return Result::kLinked;
  }
  return status == 401 ? Result::kRefused : Result::kFailed;
}

void AccountLinker::Link(const std::string& home_token,
                         const std::string& profile_token,
                         Callback callback) {
  weak_factory_.InvalidateWeakPtrs();
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = MakeAccountLinkUrl(server_url_);
  request->method = "POST";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader(net::HttpRequestHeaders::kAuthorization,
                             base::StrCat({"Bearer ", home_token}));
  request->headers.SetHeader(kLinkedTokenHeader, profile_token);
  loader_ =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  loader_->SetTimeoutDuration(kRequestTimeout);
  loader_->SetAllowHttpErrorResults(true);
  loader_->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&AccountLinker::OnResponse, weak_factory_.GetWeakPtr(),
                     std::move(callback)),
      kMaxReplySize);
}

void AccountLinker::OnResponse(Callback callback,
                               std::optional<std::string> body) {
  const network::mojom::URLResponseHead* head = loader_->ResponseInfo();
  const int status = head && head->headers ? head->headers->response_code() : 0;
  loader_.reset();
  std::move(callback).Run(ResultOf(status));
}

}  // namespace compass::sync
