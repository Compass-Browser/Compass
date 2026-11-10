// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/live_folders/live_fetcher.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "base/functional/bind.h"
#include "base/i18n/icu_string_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "net/base/net_errors.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace compass {

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_live_folders", R"(
        semantics {
          sender: "Compass live folders"
          description:
            "Fetches what a live folder shows (a feed's posts, pull requests, "
            "a calendar's events) for the plugin the user picked for it."
          trigger: "A live folder refreshes: on its timer, when it's "
                   "expanded, or when the user asks."
          data: "What the plugin requests: its settings, and the secrets "
                "the user entered for it (a token, a private address)."
          destination: OTHER
          destination_other: "The hosts the plugin's manifest declares, "
                             "and only those."
        }
        policy {
          cookies_allowed: YES
          cookies_store: "user"
          setting: "Only for live folders the user created. Cookies go only "
                   "to hosts the plugin declares for them, after the user "
                   "allows it for that folder."
          policy_exception_justification: "Not implemented."
        })");

// Headers a plugin can't set: the host owns them.
constexpr auto kForbiddenHeaders = std::to_array<std::string_view>({
    "cookie", "cookie2", "host", "origin", "referer", "content-length",
    "connection", "keep-alive", "transfer-encoding", "upgrade", "te",
    "proxy-authorization", "set-cookie",
});

bool IsForbiddenHeader(std::string_view name) {
  const std::string lower = base::ToLowerASCII(name);
  return lower.starts_with("sec-") ||
         std::ranges::find(kForbiddenHeaders, lower) != kForbiddenHeaders.end();
}

std::string DecodeBody(const std::string& bytes, const std::string& charset) {
  std::string decoded;
  if (!charset.empty() &&
      base::ConvertToUtf8AndNormalize(bytes, charset, &decoded)) {
    return decoded;
  }
  if (base::IsStringUTF8AllowingNoncharacters(bytes)) {
    return bytes;
  }
  // Invalid UTF-8 becomes U+FFFD, as TextDecoder does.
  return base::UTF16ToUTF8(base::UTF8ToUTF16(bytes));
}

bool InCookieHosts(const LiveAllowedHosts& allowed, const GURL& url) {
  return std::ranges::any_of(allowed.cookies, [&url](const std::string& p) {
    return LiveHostMatches(p, url.host());
  });
}

}  // namespace

LiveFetchRequest::LiveFetchRequest() = default;
LiveFetchRequest::LiveFetchRequest(const LiveFetchRequest&) = default;
LiveFetchRequest& LiveFetchRequest::operator=(const LiveFetchRequest&) =
    default;
LiveFetchRequest::~LiveFetchRequest() = default;

LiveFetchResponse::LiveFetchResponse() = default;
LiveFetchResponse::LiveFetchResponse(LiveFetchResponse&&) = default;
LiveFetchResponse& LiveFetchResponse::operator=(LiveFetchResponse&&) = default;
LiveFetchResponse::~LiveFetchResponse() = default;

struct LiveFetcher::Pending {
  std::unique_ptr<network::SimpleURLLoader> loader;
  Callback callback;
  GURL url;
  bool cookies = false;
  int redirects = 0;
};

LiveFetcher::LiveFetcher(
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    LiveAllowedHosts allowed,
    bool cookies_granted,
    Limits limits)
    : loader_factory_(std::move(loader_factory)),
      allowed_(std::move(allowed)),
      cookies_granted_(cookies_granted),
      limits_(limits) {}

LiveFetcher::~LiveFetcher() = default;

void LiveFetcher::Fetch(const LiveFetchRequest& request, Callback callback) {
  auto fail = [&callback](std::string code, std::string message) {
    std::move(callback).Run(
        base::unexpected(LiveError{std::move(code), std::move(message)}));
  };
  if (fetches_.size() >= limits_.max_fetches) {
    return fail("too_many_fetches",
                "More than " + base::NumberToString(limits_.max_fetches) +
                    " fetches in one refresh");
  }
  const GURL url(request.url);
  if (!url.is_valid()) {
    return fail("bad_request", "Not a URL: " + request.url);
  }
  if (!LiveUrlAllowed(url, allowed_.hosts)) {
    return fail("host_not_allowed",
                std::string(url.host()) + " is not in the plugin's hosts");
  }
  if (request.method != "GET" && request.method != "HEAD" &&
      request.method != "POST") {
    return fail("bad_request",
                "Method " + request.method + " is not allowed");
  }

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = url;
  resource_request->method = request.method;
  std::string content_type = "text/plain;charset=UTF-8";
  for (const auto& [name, value] : request.headers) {
    if (IsForbiddenHeader(name)) {
      continue;
    }
    if (!net::HttpUtil::IsValidHeaderName(name) ||
        !net::HttpUtil::IsValidHeaderValue(value)) {
      return fail("bad_request", "Invalid header " + name);
    }
    if (base::EqualsCaseInsensitiveASCII(name, "content-type")) {
      content_type = value;
      continue;
    }
    resource_request->headers.SetHeader(name, value);
  }
  const bool cookies = request.include_credentials && cookies_granted_ &&
                       InCookieHosts(allowed_, url);
  if (cookies) {
    resource_request->credentials_mode =
        network::mojom::CredentialsMode::kInclude;
    // As if the user opened the host's own page, so SameSite cookies go too.
    resource_request->site_for_cookies = net::SiteForCookies::FromUrl(url);
  } else {
    resource_request->credentials_mode =
        network::mojom::CredentialsMode::kOmit;
  }
  fetches_.push_back({request.method, url, cookies});

  auto pending = std::make_unique<Pending>();
  Pending* raw = pending.get();
  raw->callback = std::move(callback);
  raw->url = url;
  raw->cookies = cookies;
  raw->loader = network::SimpleURLLoader::Create(std::move(resource_request),
                                                 kTrafficAnnotation);
  raw->loader->SetAllowHttpErrorResults(true);
  if (request.method == "POST" && request.body) {
    raw->loader->AttachStringForUpload(*request.body, content_type);
  }
  raw->loader->SetOnRedirectCallback(base::BindRepeating(
      [](base::WeakPtr<LiveFetcher> fetcher, Pending* pending,
         const GURL& url_before_redirect,
         const net::RedirectInfo& redirect_info,
         const network::mojom::URLResponseHead& response_head,
         std::vector<std::string>* removed_headers) {
        if (fetcher) {
          fetcher->OnRedirect(pending, redirect_info.new_url);
        }
      },
      weak_factory_.GetWeakPtr(), raw));
  pending_.push_back(std::move(pending));
  raw->loader->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&LiveFetcher::OnComplete, weak_factory_.GetWeakPtr(),
                     raw),
      limits_.max_response_bytes);
}

void LiveFetcher::OnRedirect(Pending* pending, const GURL& new_url) {
  if (++pending->redirects > limits_.max_redirects) {
    // Finish() deletes the loader, which stops the redirect: SimpleURLLoader
    // allows that from this callback.
    return Finish(pending,
                  base::unexpected(LiveError{"network", "Too many redirects"}));
  }
  if (!LiveUrlAllowed(new_url, allowed_.hosts)) {
    return Finish(pending, base::unexpected(LiveError{
                               "host_not_allowed",
                               "Redirect to " + std::string(new_url.host()) +
                                   " is not allowed"}));
  }
  // Cookies stay with the hosts they were granted for.
  if (pending->cookies && !InCookieHosts(allowed_, new_url)) {
    return Finish(pending, base::unexpected(LiveError{
                               "host_not_allowed",
                               "Redirect to " + std::string(new_url.host()) +
                                   " would send it cookies"}));
  }
  pending->url = new_url;
}

void LiveFetcher::OnComplete(Pending* pending,
                             std::optional<std::string> body) {
  const network::mojom::URLResponseHead* head = pending->loader->ResponseInfo();
  if (!body || !head || !head->headers) {
    const int error = pending->loader->NetError();
    if (error == net::ERR_INSUFFICIENT_RESOURCES) {
      return Finish(pending,
                    base::unexpected(LiveError{
                        "response_too_large",
                        "Response from " + std::string(pending->url.host()) + " is over " +
                            base::NumberToString(limits_.max_response_bytes) +
                            " bytes"}));
    }
    return Finish(pending, base::unexpected(LiveError{
                               "network", "Couldn't reach " +
                                              std::string(pending->url.host()) + " (" +
                                              net::ErrorToShortString(error) +
                                              ")"}));
  }
  LiveFetchResponse response;
  response.status = head->headers->response_code();
  response.url = pending->loader->GetFinalURL();
  size_t iter = 0;
  std::string name;
  std::string value;
  while (head->headers->EnumerateHeaderLines(&iter, &name, &value)) {
    std::string& joined = response.headers[base::ToLowerASCII(name)];
    joined = joined.empty() ? value : joined + ", " + value;
  }
  response.body = DecodeBody(*body, head->charset);
  Finish(pending, std::move(response));
}

void LiveFetcher::Finish(Pending* pending,
                         base::expected<LiveFetchResponse, LiveError> result) {
  auto it = std::ranges::find(pending_, pending,
                              &std::unique_ptr<Pending>::get);
  CHECK(it != pending_.end());
  std::unique_ptr<Pending> owned = std::move(*it);
  pending_.erase(it);
  Callback callback = std::move(owned->callback);
  // Deletes the loader.
  owned.reset();
  std::move(callback).Run(std::move(result));
}

}  // namespace compass
