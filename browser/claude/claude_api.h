// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_API_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_API_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compass/browser/claude/ai_provider.h"
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

// Whether |key| is such a token, not an API key (only Anthropic's endpoints
// ever take it as one). With the user's
// "Accept the risks of subscription tokens" (Endpoint::allow_oauth_token) it
// goes as "Authorization: Bearer" with kOAuthBeta whatever the auth style
// says; without, it isn't sent (NeedsTokenRisksAccepted()).
bool IsOAuthToken(std::string_view key);

// Where requests go and how they authenticate.
struct Endpoint {
  Endpoint();
  Endpoint(const Endpoint&);
  Endpoint& operator=(const Endpoint&);
  ~Endpoint();

  // Who answers (docs/design/ai-providers.md): another provider than
  // Anthropic gets its own paths and headers here, and its bodies and
  // streams translated (openai_edge.h).
  AIProvider provider = AIProvider::kAnthropic;
  GURL base_url;
  AuthStyle auth_style = AuthStyle::kApiKey;
  std::string api_key;
  // The user accepted the risks of subscription tokens
  // (prefs::kAcceptTokenRisks).
  bool allow_oauth_token = false;
  // OpenAI on the user's ChatGPT plan (Sign in with ChatGPT): api_key is the
  // access token, and base_url OpenAI's own.
  bool chatgpt = false;
};

// Whether the profile's OpenAI requests go on the user's ChatGPT plan: OpenAI
// is the provider, and signed in with ChatGPT (ChatGPTAccount).
bool UsesChatGPT(const PrefService& prefs);

// Whether requests send |endpoint|'s key as a subscription token: it is one,
// and the user accepted the risks.
bool UsesOAuthToken(const Endpoint& endpoint);

// Whether |endpoint|'s key is a subscription token the user hasn't accepted
// the risks of: nothing is sent, and IDS_COMPASS_AI_TOKEN_RISKS (or
// IDS_COMPASS_AI_TOKEN_RISKS_ANDROID) says why.
bool NeedsTokenRisksAccepted(const Endpoint& endpoint);

// The base URL the user typed, normalized: https (http only to this
// computer, for local gateways and tests), no query or fragment, no trailing
// slash. A path is kept, for gateways that serve the API under one. Empty
// text means |provider|'s own (DefaultBaseUrl()); none for kCompatible, which
// needs one. Null if it isn't such a URL.
std::optional<GURL> ParseBaseUrl(
    std::string_view text,
    AIProvider provider = AIProvider::kAnthropic);

// |path| ("v1/messages") under |base_url|, after its own path.
GURL EndpointUrl(const GURL& base_url, std::string_view path);

// The chat's path under the base URL: "v1/messages", "responses" (OpenAI)
// or "chat/completions".
std::string_view ChatPath(AIProvider provider);
// The model list's URL ("v1/models?limit=100", or "models").
GURL ModelsUrl(const Endpoint& endpoint);

// The headers every request carries. Another provider: "Authorization:
// Bearer <key>" (none without a key, for a local server), nothing else.
// Anthropic: the version and the key (and, for an
// OAuth token, kOAuthBeta; add the request's own betas with AddBetas()).
void AddRequestHeaders(const Endpoint& endpoint,
                       net::HttpRequestHeaders& headers);

// Anthropic only: sets "anthropic-beta" to |betas|, and kOAuthBeta too if |endpoint|'s key is
// an OAuth token. Sets nothing if that comes to no beta.
void AddBetas(const Endpoint& endpoint,
              const std::vector<std::string>& betas,
              net::HttpRequestHeaders& headers);

// The endpoint the profile's settings describe, for the provider they name
// (the key not included; it's stored encrypted, see ClaudeCredentials). An
// OpenAI-compatible server without a URL has an empty base_url.
Endpoint EndpointFromPrefs(const PrefService& prefs);

// How many cache_control breakpoints a chat request carries. A gateway (a
// custom base URL) can add its own and get the request refused ("A maximum
// of 4 blocks with cache_control may be provided. Found 5."): the chat then
// steps down, kNormal → kTopLevelOnly → kNone, and remembers for the
// endpoint the level that got through.
enum class CacheLevel {
  // The system prompt's last block, and the top-level (automatic) one.
  kNormal = 0,
  // Only the top-level one, which caches the whole prefix too.
  kTopLevelOnly = 1,
  // No cache_control at all.
  kNone = 2,
};
// The next step down, or nullopt at kNone.
std::optional<CacheLevel> LowerCacheLevel(CacheLevel level);

// Whether |error| is a refusal for too many cache_control blocks, in the
// API's words or a gateway's (any case, wrapped in its own message or JSON).
bool IsTooManyCacheBreakpoints(std::string_view error);

// The cache level for chats with |base_url|: the one that got through after
// a refusal (prefs::kCacheLevelUrl, prefs::kCacheLevel), else kNormal. Never
// below kNormal for the Claude API itself.
CacheLevel CacheLevelFor(const PrefService& prefs, const GURL& base_url);
// Remembers |level| for |base_url| (one endpoint, the one in use); kNormal,
// or the Claude API, forgets it.
void RememberCacheLevel(PrefService& prefs,
                        const GURL& base_url,
                        CacheLevel level);

struct ModelInfo {
  std::string id;
  std::string display_name;
};

// The models in a GET /v1/models reply, as the API lists them (newest
// first). On the ChatGPT plan the reply is {"models": [{"slug",
// "display_name", "visibility"}]}: the listed ones, in its order. Another provider's list is sorted by id; OpenAI's is cut to chat
// models (no embeddings, audio, images, moderation, realtime...), Gemini's
// ids lose "models/". Null if |json| isn't one.
std::optional<std::vector<ModelInfo>> ParseModelList(
    std::string_view json,
    AIProvider provider = AIProvider::kAnthropic);

// What went wrong, for the user: the API's own message for an error reply
// ({"type": "error", "error": {"type": ..., "message": ...}}), otherwise
// what the status means, in |provider|'s words ("The server is overloaded",
// not "Claude is overloaded"). Gemini's [{"error": …}] too. |http_status| 0
// is a network error.
std::string DescribeError(int http_status,
                          std::string_view body,
                          AIProvider provider = AIProvider::kAnthropic);

// What settings show of a saved key: its start and last four characters
// ("sk-ant-…a1b2"), never the whole of it.
std::string KeyHint(std::string_view key);

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_API_H_
