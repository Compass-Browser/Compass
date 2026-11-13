// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_api.h"

#include <algorithm>

#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "compass/browser/claude/claude_prefs.h"
#include "components/prefs/pref_service.h"
#include "net/base/url_util.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_status_code.h"

namespace compass::claude {

Endpoint::Endpoint() = default;
Endpoint::Endpoint(const Endpoint&) = default;
Endpoint& Endpoint::operator=(const Endpoint&) = default;
Endpoint::~Endpoint() = default;

std::optional<GURL> ParseBaseUrl(std::string_view text) {
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(text, base::TRIM_ALL);
  if (trimmed.empty()) {
    return GURL(kDefaultBaseUrl);
  }
  GURL url(trimmed);
  if (!url.is_valid() || url.has_query() || url.has_ref() ||
      url.has_username() || url.has_password()) {
    return std::nullopt;
  }
  // The key goes along with every request: only over TLS, or to this
  // computer.
  if (!url.SchemeIs("https") &&
      !(url.SchemeIs("http") && net::IsLocalhost(url))) {
    return std::nullopt;
  }
  std::string path(url.path());
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  if (path == "/") {
    path.clear();
  }
  // Replacements keep a view of |path|, which outlives them.
  GURL::Replacements replacements;
  replacements.SetPathStr(path);
  return url.ReplaceComponents(replacements);
}

GURL EndpointUrl(const GURL& base_url, std::string_view path) {
  std::string base_path(base_url.path());
  if (base_path.empty() || base_path.back() != '/') {
    base_path.push_back('/');
  }
  GURL::Replacements replacements;
  const std::string full = base::StrCat({base_path, path});
  replacements.SetPathStr(full);
  return base_url.ReplaceComponents(replacements);
}

bool IsOAuthToken(std::string_view key) {
  return base::StartsWith(base::TrimWhitespaceASCII(key, base::TRIM_ALL),
                          "sk-ant-oat");
}

bool UsesOAuthToken(const Endpoint& endpoint) {
  return endpoint.allow_oauth_token && IsOAuthToken(endpoint.api_key);
}

bool NeedsTokenRisksAccepted(const Endpoint& endpoint) {
  return !endpoint.allow_oauth_token && IsOAuthToken(endpoint.api_key);
}

void AddRequestHeaders(const Endpoint& endpoint,
                       net::HttpRequestHeaders& headers) {
  headers.SetHeader("anthropic-version", kApiVersion);
  if (endpoint.auth_style == AuthStyle::kBearer || UsesOAuthToken(endpoint)) {
    headers.SetHeader(net::HttpRequestHeaders::kAuthorization,
                      base::StrCat({"Bearer ", endpoint.api_key}));
  } else {
    headers.SetHeader("x-api-key", endpoint.api_key);
  }
}

void AddBetas(const Endpoint& endpoint,
              const std::vector<std::string>& betas,
              net::HttpRequestHeaders& headers) {
  std::vector<std::string> all = betas;
  if (UsesOAuthToken(endpoint) && !std::ranges::contains(all, kOAuthBeta)) {
    all.push_back(kOAuthBeta);
  }
  if (!all.empty()) {
    headers.SetHeader("anthropic-beta", base::JoinString(all, ","));
  }
}

Endpoint EndpointFromPrefs(const PrefService& prefs) {
  Endpoint endpoint;
  endpoint.base_url =
      ParseBaseUrl(prefs.GetString(prefs::kBaseUrl)).value_or(
          GURL(kDefaultBaseUrl));
  endpoint.auth_style = prefs.GetInteger(prefs::kAuthStyle) ==
                                static_cast<int>(AuthStyle::kBearer)
                            ? AuthStyle::kBearer
                            : AuthStyle::kApiKey;
  endpoint.allow_oauth_token = prefs.GetBoolean(prefs::kAcceptTokenRisks);
  return endpoint;
}

std::optional<CacheLevel> LowerCacheLevel(CacheLevel level) {
  switch (level) {
    case CacheLevel::kNormal:
      return CacheLevel::kTopLevelOnly;
    case CacheLevel::kTopLevelOnly:
      return CacheLevel::kNone;
    case CacheLevel::kNone:
      return std::nullopt;
  }
  return std::nullopt;
}

bool IsTooManyCacheBreakpoints(std::string_view error) {
  const std::string text = base::ToLowerASCII(error);
  if (text.find("cache_control") == std::string::npos &&
      text.find("cache control") == std::string::npos) {
    return false;
  }
  for (std::string_view word :
       {"maximum", "blocks", "breakpoint", "too many", "at most", "limit"}) {
    if (text.find(word) != std::string::npos) {
      return true;
    }
  }
  return false;
}

CacheLevel CacheLevelFor(const PrefService& prefs, const GURL& base_url) {
  const std::string& remembered = prefs.GetString(prefs::kCacheLevelUrl);
  if (remembered.empty() || base_url == GURL(kDefaultBaseUrl) ||
      remembered != base_url.spec()) {
    return CacheLevel::kNormal;
  }
  switch (prefs.GetInteger(prefs::kCacheLevel)) {
    case static_cast<int>(CacheLevel::kTopLevelOnly):
      return CacheLevel::kTopLevelOnly;
    case static_cast<int>(CacheLevel::kNone):
      return CacheLevel::kNone;
    default:
      return CacheLevel::kNormal;
  }
}

void RememberCacheLevel(PrefService& prefs,
                        const GURL& base_url,
                        CacheLevel level) {
  if (level == CacheLevel::kNormal || base_url == GURL(kDefaultBaseUrl)) {
    prefs.ClearPref(prefs::kCacheLevelUrl);
    prefs.ClearPref(prefs::kCacheLevel);
    return;
  }
  prefs.SetString(prefs::kCacheLevelUrl, base_url.spec());
  prefs.SetInteger(prefs::kCacheLevel, static_cast<int>(level));
}

std::optional<std::vector<ModelInfo>> ParseModelList(std::string_view json) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict) {
    return std::nullopt;
  }
  const base::ListValue* data = dict->FindList("data");
  if (!data) {
    return std::nullopt;
  }
  std::vector<ModelInfo> models;
  for (const base::Value& item : *data) {
    const base::DictValue* model = item.GetIfDict();
    const std::string* id = model ? model->FindString("id") : nullptr;
    if (!id || id->empty()) {
      continue;
    }
    const std::string* name = model->FindString("display_name");
    models.push_back({*id, name && !name->empty() ? *name : *id});
  }
  return models;
}

std::string DescribeError(int http_status, std::string_view body) {
  if (http_status == 0) {
    return "Couldn't reach the server. Check the connection and the "
           "endpoint URL.";
  }
  if (std::optional<base::DictValue> dict =
          base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC)) {
    if (const base::DictValue* error = dict->FindDict("error")) {
      const std::string* message = error->FindString("message");
      if (message && !message->empty()) {
        return *message;
      }
    }
    // A gateway's own shapes: {"message": …}, {"error": "…"}, {"detail": …}.
    for (const char* key : {"message", "error", "detail"}) {
      const std::string* message = dict->FindString(key);
      if (message && !message->empty()) {
        return *message;
      }
    }
  }
  // Or plain text: kept when it's about cache breakpoints, so the chat can
  // step down (IsTooManyCacheBreakpoints()).
  if (IsTooManyCacheBreakpoints(body.substr(0, 2048))) {
    return std::string(base::TrimWhitespaceASCII(body.substr(0, 2048),
                                                 base::TRIM_ALL));
  }
  switch (http_status) {
    case net::HTTP_UNAUTHORIZED:
      return "The key was refused (401). Check it, and the auth style. A "
             "token from `claude setup-token` may have expired: make a new "
             "one.";
    case net::HTTP_FORBIDDEN:
      return "The key isn't allowed to do this (403).";
    case net::HTTP_NOT_FOUND:
      return "Nothing answers at that URL (404). Check the endpoint URL.";
    case net::HTTP_TOO_MANY_REQUESTS:
      return "Too many requests (429). Wait a moment and try again.";
    case 529:
      return "Claude is overloaded (529). Try again in a moment.";
    default:
      return base::StrCat(
          {"The server answered with HTTP ", base::NumberToString(http_status),
           "."});
  }
}

std::string KeyHint(std::string_view key) {
  key = base::TrimWhitespaceASCII(key, base::TRIM_ALL);
  if (key.size() < 12) {
    return "…";
  }
  // "sk-ant-" for Claude API keys; whatever the first seven are otherwise.
  return base::StrCat({key.substr(0, 7), "…", key.substr(key.size() - 4)});
}

}  // namespace compass::claude
