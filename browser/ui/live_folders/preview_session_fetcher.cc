// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/live_folders/preview_session_fetcher.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace compass {

namespace {

bool IsRequestUrl(const GURL& url) {
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS() && !url.has_username() &&
         !url.has_password();
}

bool MatchesAny(const std::vector<std::string>& patterns, const GURL& url) {
  return std::ranges::any_of(patterns, [&url](const std::string& pattern) {
    return LiveHostMatches(pattern, url.host());
  });
}

bool OnTabSite(const GURL& tab_url, const GURL& url) {
  const std::string site = LiveSiteOf(tab_url.host());
  return !site.empty() && LiveSiteOf(url.host()) == site;
}

}  // namespace

PreviewHostKind ClassifyPreviewUrl(const LivePluginManifest::Preview& preview,
                                   const GURL& tab_url,
                                   const GURL& url) {
  if (!preview.session || !IsRequestUrl(url) || !IsRequestUrl(tab_url)) {
    return PreviewHostKind::kRefused;
  }
  const LivePluginManifest::PreviewSession& session = *preview.session;
  if (OnTabSite(tab_url, url)) {
    return session.hosts.empty() || MatchesAny(session.hosts, url)
               ? PreviewHostKind::kOwnSite
               : PreviewHostKind::kRefused;
  }
  return MatchesAny(session.related, url) ? PreviewHostKind::kRelated
                                          : PreviewHostKind::kRefused;
}

bool PreviewIconAllowed(const LivePluginManifest::Preview& preview,
                        const GURL& tab_url,
                        bool session_granted,
                        const GURL& icon) {
  if (!IsRequestUrl(icon) || !IsRequestUrl(tab_url)) {
    return false;
  }
  if (OnTabSite(tab_url, icon)) {
    return true;
  }
  return session_granted && preview.session &&
         MatchesAny(preview.session->related, icon);
}

PreviewPageTransport::Response::Response() = default;
PreviewPageTransport::Response::Response(Response&&) = default;
PreviewPageTransport::Response& PreviewPageTransport::Response::operator=(
    Response&&) = default;
PreviewPageTransport::Response::~Response() = default;

PreviewSessionFetcher::PreviewSessionFetcher(
    LivePluginManifest::Preview preview,
    GURL tab_url,
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    std::unique_ptr<PreviewPageTransport> page,
    Limits limits)
    : preview_(std::move(preview)),
      tab_url_(std::move(tab_url)),
      limits_(limits),
      page_(std::move(page)) {
  // The browser's side: hops are checked against the same hosts, cookies go
  // only to the own site, and a redirect that would carry them elsewhere is
  // refused (and sent again without them below).
  LiveAllowedHosts allowed;
  if (preview_.session) {
    std::vector<std::string> own = preview_.session->hosts;
    if (own.empty()) {
      const std::string site = LiveSiteOf(tab_url_.host());
      if (!site.empty()) {
        own.push_back("*." + site);
      }
    }
    allowed.hosts = own;
    allowed.hosts.insert(allowed.hosts.end(), preview_.session->related.begin(),
                         preview_.session->related.end());
    allowed.cookies = std::move(own);
  }
  LiveFetcher::Limits fetcher_limits;
  // This class counts requests; the fetcher sees re-sends too.
  fetcher_limits.max_fetches = limits_.max_requests * 3;
  fetcher_limits.max_response_bytes = limits_.max_response_bytes;
  fetcher_limits.max_redirects = limits_.max_redirects;
  browser_ = std::make_unique<LiveFetcher>(std::move(loader_factory),
                                           std::move(allowed),
                                           /*cookies_granted=*/true,
                                           fetcher_limits);
}

PreviewSessionFetcher::~PreviewSessionFetcher() = default;

const std::vector<LiveFetchRecord>& PreviewSessionFetcher::fetches() const {
  return records_;
}

void PreviewSessionFetcher::Fetch(const LiveFetchRequest& request,
                                  Callback callback) {
  auto fail = [&callback](std::string code, std::string message) {
    std::move(callback).Run(
        base::unexpected(LiveError{std::move(code), std::move(message)}));
  };
  if (request.method != "GET" && request.method != "HEAD" &&
      request.method != "POST") {
    return fail("bad_request", "Method " + request.method + " is not allowed");
  }
  if (requests_ >= limits_.max_requests) {
    return fail("too_many_fetches",
                "More than " + base::NumberToString(limits_.max_requests) +
                    " requests in one preview");
  }
  const GURL url(request.url);
  if (!url.is_valid()) {
    return fail("bad_request", "Not a URL: " + request.url);
  }
  const PreviewHostKind kind = ClassifyPreviewUrl(preview_, tab_url_, url);
  if (kind == PreviewHostKind::kRefused) {
    return fail("host_not_allowed", std::string(url.host()) +
                                        " is not the tab's site or a related "
                                        "host");
  }
  ++requests_;
  LiveFetchRequest sent = request;
  sent.url = url.spec();
  sent.include_credentials = kind == PreviewHostKind::kOwnSite;
  std::erase_if(sent.headers, [](const auto& header) {
    return IsForbiddenHeader(header.first);
  });
  const bool from_page =
      sent.include_credentials && page_ && !request.prefer_browser;
  records_.push_back({sent.method, url, sent.include_credentials,
                      from_page ? "page" : "browser", false});
  if (from_page) {
    page_->Fetch(sent, limits_.max_response_bytes,
                 base::BindOnce(&PreviewSessionFetcher::OnFromPage,
                                weak_factory_.GetWeakPtr(), sent,
                                std::move(callback)));
    return;
  }
  FromBrowser(std::move(sent), std::move(callback));
}

void PreviewSessionFetcher::OnFromPage(
    LiveFetchRequest request,
    Callback callback,
    base::expected<PreviewPageTransport::Response, LiveError> result) {
  if (!result.has_value()) {
    std::move(callback).Run(base::unexpected(std::move(result.error())));
    return;
  }
  if (result->redirect) {
    records_.push_back({request.method, GURL(request.url), true, "browser",
                        true});
    FromBrowser(std::move(request), std::move(callback));
    return;
  }
  if (result->response.body.size() > limits_.max_response_bytes) {
    std::move(callback).Run(base::unexpected(LiveError{
        "response_too_large", "Response from " +
                                  std::string(GURL(request.url).host()) +
                                  " is too large"}));
    return;
  }
  std::move(callback).Run(std::move(result->response));
}

void PreviewSessionFetcher::FromBrowser(LiveFetchRequest request,
                                        Callback callback) {
  LiveFetchRequest copy = request;
  browser_->Fetch(copy, base::BindOnce(&PreviewSessionFetcher::OnFromBrowser,
                                       weak_factory_.GetWeakPtr(),
                                       std::move(request), std::move(callback)));
}

void PreviewSessionFetcher::OnFromBrowser(
    LiveFetchRequest request,
    Callback callback,
    base::expected<LiveFetchResponse, LiveError> result) {
  // A redirect from the own site to a related host: follow it, without the
  // session's cookies, and never give them back after it.
  if (!result.has_value() && request.include_credentials &&
      result.error().cookie_redirect.is_valid() &&
      ClassifyPreviewUrl(preview_, tab_url_, result.error().cookie_redirect) ==
          PreviewHostKind::kRelated) {
    LiveFetchRequest next = request;
    next.url = result.error().cookie_redirect.spec();
    next.include_credentials = false;
    if (next.method == "POST") {
      next.method = "GET";
      next.body.reset();
    }
    records_.push_back({next.method, result.error().cookie_redirect, false,
                        "browser", true});
    FromBrowser(std::move(next), std::move(callback));
    return;
  }
  std::move(callback).Run(std::move(result));
}

}  // namespace compass
