// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/cloudflare_oauth.h"

#include <utility>

#include "base/base64url.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/rand_util.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "crypto/hash.h"
#include "net/base/url_util.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace compass::sync {
namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_sync_cloudflare_sign_in", R"(
        semantics {
          sender: "Compass sync"
          description:
            "After the user signs in to Cloudflare to set up their own sync "
            "server, trades the one-time code Cloudflare gave for an access "
            "token, which Compass uses once to set up the server."
          trigger: "The user signs in with Cloudflare on the sync page."
          data: "The one-time code, its PKCE verifier and Compass's client "
                "ID."
          destination: OTHER
          destination_other: "Cloudflare (dash.cloudflare.com)."
        }
        policy {
          cookies_allowed: NO
          setting: "Only when the user sets up a server this way."
          policy_exception_justification: "Not implemented."
        })");

std::string RandomBase64Url(size_t bytes) {
  std::string out;
  base::Base64UrlEncode(base::RandBytesAsVector(bytes),
                        base::Base64UrlEncodePolicy::OMIT_PADDING, &out);
  return out;
}

}  // namespace

CloudflareAuthRequest MakeCloudflareAuthRequest() {
  CloudflareAuthRequest r;
  r.verifier = RandomBase64Url(48);  // 64 characters.
  r.state = RandomBase64Url(24);     // 32 characters (Cloudflare wants >= 8).
  std::string challenge;
  base::Base64UrlEncode(crypto::hash::Sha256(r.verifier),
                        base::Base64UrlEncodePolicy::OMIT_PADDING, &challenge);
  GURL url(kCloudflareAuthorizeUrl);
  url = net::AppendQueryParameter(url, "response_type", "code");
  url = net::AppendQueryParameter(url, "client_id", kCloudflareClientId);
  url = net::AppendQueryParameter(url, "redirect_uri", kCloudflareRedirectUri);
  url = net::AppendQueryParameter(url, "scope", kCloudflareScopes);
  url = net::AppendQueryParameter(url, "state", r.state);
  url = net::AppendQueryParameter(url, "code_challenge", challenge);
  url = net::AppendQueryParameter(url, "code_challenge_method", "S256");
  r.url = url;
  return r;
}

bool IsCloudflareRedirect(const GURL& url) {
  const GURL redirect(kCloudflareRedirectUri);
  return url.is_valid() && url.scheme() == redirect.scheme() &&
         url.host() == redirect.host() && url.EffectiveIntPort() ==
                                              redirect.EffectiveIntPort() &&
         url.GetPath() == redirect.GetPath();
}

CloudflareAuthReply ParseCloudflareRedirect(const GURL& url,
                                            std::string_view expected_state) {
  CloudflareAuthReply reply;
  std::string state, code, error;
  net::GetValueForKeyInQuery(url, "state", &state);
  net::GetValueForKeyInQuery(url, "code", &code);
  net::GetValueForKeyInQuery(url, "error", &error);
  if (expected_state.empty() || state != expected_state) {
    reply.error = "state";
    return reply;
  }
  if (!error.empty()) {
    reply.error = error;
    return reply;
  }
  if (code.empty()) {
    reply.error = "no_code";
    return reply;
  }
  reply.code = code;
  return reply;
}

CloudflareTokenExchange::CloudflareTokenExchange(
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    GURL token_url)
    : loader_factory_(std::move(loader_factory)),
      token_url_(std::move(token_url)) {}

CloudflareTokenExchange::~CloudflareTokenExchange() = default;

void CloudflareTokenExchange::Exchange(const std::string& code,
                                       const std::string& verifier,
                                       Callback callback) {
  auto form = [](std::string_view v) {
    return base::EscapeQueryParamValue(v, /*use_plus=*/true);
  };
  const std::string body = base::StrCat(
      {"grant_type=authorization_code&code=", form(code),
       "&redirect_uri=", form(kCloudflareRedirectUri),
       "&client_id=", form(kCloudflareClientId),
       "&code_verifier=", form(verifier)});
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = token_url_;
  request->method = "POST";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->SetTimeoutDuration(base::Seconds(30));
  loader_->SetAllowHttpErrorResults(true);
  loader_->AttachStringForUpload(body, "application/x-www-form-urlencoded");
  loader_->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&CloudflareTokenExchange::OnResponse,
                     weak_factory_.GetWeakPtr(), std::move(callback)),
      64 * 1024);
}

void CloudflareTokenExchange::OnResponse(Callback callback,
                                         std::optional<std::string> body) {
  const network::mojom::URLResponseHead* head = loader_->ResponseInfo();
  const int status =
      head && head->headers ? head->headers->response_code() : 0;
  loader_.reset();
  if (status != 200 || !body) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  std::optional<base::Value> json =
      base::JSONReader::Read(*body, base::JSON_PARSE_RFC);
  const std::string* token =
      json && json->is_dict() ? json->GetDict().FindString("access_token")
                              : nullptr;
  if (!token || token->empty()) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  std::move(callback).Run(*token);
}

}  // namespace compass::sync
