// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_API_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_API_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "url/gurl.h"

namespace net {
class HttpRequestHeaders;
}  // namespace net

class PrefService;

// The Claude API's endpoints and formats, as Compass talks to them: no SDK,
// the Messages API over HTTP. See docs/design/claude.md.
namespace compass::claude {

inline constexpr char kDefaultBaseUrl[] = "https://api.anthropic.com";
// The API version every request names (anthropic-version).
inline constexpr char kApiVersion[] = "2026-10-21";
// The model a new profile chats with; settings offer the others.
inline constexpr char kDefaultModel[] = "claude-opus-5-5";

// How the key is sent: "x-api-key: <key>" (the Claude API), or
// "Authorization: Bearer <key>" (gateways that take a token).
enum class AuthStyle {
  kApiKey = 0,
  kBearer = 1,
};

// The beta that lets the API take an OAuth token, which is what
// `claude setup-token` prints (sk-ant-oat01-…).
inline constexpr char kOAuthBeta[] = "oauth-2025-04-20";

// Whether |key| is such a token, not an API key: it goes as
// "Authorization: Bearer" with kOAuthBeta whatever the auth style says.
bool IsOAuthToken(std::string_view key);

// Where requests go and how they authenticate.
struct Endpoint {
  Endpoint();
  Endpoint(const Endpoint&);
  Endpoint& operator=(const Endpoint&);
  ~Endpoint();

  GURL base_url;
  AuthStyle auth_style = AuthStyle::kApiKey;
  std::string api_key;
};

// The base URL the user typed, normalized: https (http only to this
// computer, for local gateways and tests), no query or fragment, no trailing
// slash. A path is kept, for gateways that serve the API under one. Empty
// text means the Claude API. Null if it isn't such a URL.
std::optional<GURL> ParseBaseUrl(std::string_view text);

// |path| ("v1/messages") under |base_url|, after its own path.
GURL EndpointUrl(const GURL& base_url, std::string_view path);

// The headers every request carries: the version and the key (and, for an
// OAuth token, kOAuthBeta; add the request's own betas with AddBetas()).
void AddRequestHeaders(const Endpoint& endpoint,
                       net::HttpRequestHeaders& headers);

// Sets "anthropic-beta" to |betas|, and kOAuthBeta too if |endpoint|'s key is
// an OAuth token. Sets nothing if that comes to no beta.
void AddBetas(const Endpoint& endpoint,
              const std::vector<std::string>& betas,
              net::HttpRequestHeaders& headers);

// The endpoint the profile's settings describe (the key not included; it's
// stored encrypted, see ClaudeCredentials).
Endpoint EndpointFromPrefs(const PrefService& prefs);

struct ModelInfo {
  std::string id;
  std::string display_name;
};

// The models in a GET /v1/models reply, as the API lists them (newest
// first). Null if |json| isn't one.
std::optional<std::vector<ModelInfo>> ParseModelList(std::string_view json);

// What went wrong, for the user: the API's own message for an error reply
// ({"type": "error", "error": {"type": ..., "message": ...}}), otherwise
// what the status means. |http_status| 0 is a network error.
std::string DescribeError(int http_status, std::string_view body);

// What settings show of a saved key: its start and last four characters
// ("sk-ant-…a1b2"), never the whole of it.
std::string KeyHint(std::string_view key);

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_API_H_
