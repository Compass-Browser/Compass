// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CHATGPT_OAUTH_H_
#define COMPASS_BROWSER_CLAUDE_CHATGPT_OAUTH_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "url/gurl.h"

// Sign in with ChatGPT, the open-source ("direct") flow: OAuth 2 with PKCE
// against auth.openai.com, a loopback redirect, and the user's ChatGPT plan
// paying for the OpenAI provider's requests (docs/design/ai-providers.md,
// "Sign in with ChatGPT"). What's here is the protocol, without I/O; the
// sign-in itself, the tokens' storage and their refresh are ChatGPTAccount's.
// iOS: CompassClaude's ChatGPTOAuth.
namespace compass::claude::chatgpt {

inline constexpr char kAuthorizeUrl[] =
    "https://auth.openai.com/api/accounts/authorize";
inline constexpr char kTokenUrl[] =
    "https://auth.openai.com/api/accounts/oauth/token";
// Names the revocation endpoint, which the docs don't give otherwise.
inline constexpr char kDiscoveryUrl[] =
    "https://auth.openai.com/.well-known/openid-configuration";
inline constexpr char kIssuer[] = "https://auth.openai.com";
// What the tokens are for, and where the requests go: the Responses API.
inline constexpr char kResource[] = "https://api.openai.com/v1";
// The identity scopes, then the plan's.
inline constexpr char kScopes[] =
    "openid profile email offline_access resource.invoke "
    "chatgpt.tokens.use.direct";
// The scope without which a sign-in can't use the plan.
inline constexpr char kPlanScope[] = "chatgpt.tokens.use.direct";
// The client_id of a first sign-in, which registers Compass for the account
// and gets it its own (oaiapp_…). Never stored or exchanged.
inline constexpr char kDynamicClientId[] = "dynamic_agent_client";
// What the consent screen calls Compass.
inline constexpr char kAgentName[] = "Compass";
// The loopback redirect: http://127.0.0.1:<port>/auth/callback. Only the
// port may change between sign-ins.
inline constexpr char kCallbackPath[] = "/auth/callback";
inline constexpr int kPreferredPort = 1455;
// Refreshed this long before it expires.
inline constexpr base::TimeDelta kRefreshMargin = base::Minutes(2);
// Where the user sees and caps what apps use of their plan.
inline constexpr char kUsageUrl[] = "https://chatgpt.com/settings/usage";

// A random value for state, nonce and the PKCE verifier: 32 bytes,
// base64url without padding.
std::string RandomToken();
// The PKCE challenge for |verifier|: base64url(SHA-256), no padding.
std::string CodeChallenge(std::string_view verifier);
// A new install's ext_agent_host_id: "urn:uuid:<v4>". Opaque, nothing about
// the user in it.
std::string NewHostId();
// The redirect URI for |port|.
std::string RedirectUri(int port);

struct AuthorizeParams {
  AuthorizeParams();
  AuthorizeParams(const AuthorizeParams&);
  AuthorizeParams& operator=(const AuthorizeParams&);
  ~AuthorizeParams();

  // The client Compass was registered as; empty for a first sign-in, which
  // registers it (kDynamicClientId, with kAgentName).
  std::string client_id;
  std::string redirect_uri;
  std::string state;
  std::string nonce;
  std::string code_challenge;
  std::string host_id;
  // Reauthorization only, both optional: the last ID token (may have
  // expired) and the account's email.
  std::string id_token_hint;
  std::string login_hint;
};

// The authorize URL the browser opens.
GURL AuthorizeUrl(const AuthorizeParams& params);

// What the redirect brought back.
struct CallbackResult {
  CallbackResult();
  CallbackResult(const CallbackResult&);
  CallbackResult& operator=(const CallbackResult&);
  ~CallbackResult();

  std::string code;
  std::string state;
  // The client Compass is registered as: always on a first sign-in, maybe
  // on later ones.
  std::string client_id;
  std::string scope;
  // "access_denied" when the user declined; empty without an error.
  std::string error;
  std::string error_description;
};

// The redirect's query.
CallbackResult ParseCallback(const GURL& url);

// The client the code is exchanged with, given the one Compass had
// (|saved_client_id|, empty for a first sign-in) and what the callback says;
// an error for the user if they don't fit (a first sign-in without an issued
// client, or a callback naming another one than Compass's).
base::expected<std::string, std::string> ClientForCallback(
    std::string_view saved_client_id,
    const CallbackResult& callback);

// The token endpoint's form bodies.
std::string CodeExchangeBody(std::string_view client_id,
                             std::string_view code,
                             std::string_view verifier,
                             std::string_view redirect_uri);
std::string RefreshBody(std::string_view client_id,
                        std::string_view refresh_token);
std::string RevokeBody(std::string_view client_id,
                       std::string_view refresh_token);

struct TokenReply {
  TokenReply();
  TokenReply(const TokenReply&);
  TokenReply& operator=(const TokenReply&);
  ~TokenReply();

  std::string access_token;
  // Each reply's replaces the one before (they rotate).
  std::string refresh_token;
  std::string id_token;
  base::TimeDelta expires_in;
  std::string scope;
};

struct TokenError {
  // The OAuth error ("invalid_grant"), or empty for a network or server
  // failure.
  std::string code;
  // For the user.
  std::string message;
  // The sign-in is over (the refresh token was refused): sign in again. Not
  // for a network or server failure, which keeps the tokens.
  bool needs_sign_in = false;
};

// The token endpoint's reply to a code exchange or a refresh. |http_status| 0
// is a network error.
base::expected<TokenReply, TokenError> ParseTokenReply(int http_status,
                                                       std::string_view body);

// Whether |scope| (space-separated) has the plan's scope.
bool HasPlanScope(std::string_view scope);

// What Compass reads from the ID token: who signed in.
struct IdTokenClaims {
  std::string subject;
  std::string email;
};

// The ID token's claims, checked: issuer, audience (|client_id|), |nonce|,
// expiry (|now|, with a few seconds' leeway) and a subject. Not its
// signature: it came straight from the token endpoint over TLS, which OpenID
// Connect Core (3.1.3.7) allows instead, and it only names the account.
base::expected<IdTokenClaims, std::string> CheckIdToken(
    std::string_view id_token,
    std::string_view client_id,
    std::string_view nonce,
    base::Time now);

// A signed-in account's tokens, as stored (encrypted) between runs.
struct Session {
  Session();
  Session(const Session&);
  Session& operator=(const Session&);
  ~Session();

  std::string client_id;
  std::string access_token;
  std::string refresh_token;
  std::string id_token;
  base::Time expires_at;
  std::string scope;
  std::string email;
  std::string subject;

  // A refresh's reply replaces the tokens, the expiry and the scope
  // together; a reply without a refresh token keeps the one before.
  void Apply(const TokenReply& reply, base::Time now);
  // Whether the access token is good for kRefreshMargin more.
  bool IsFresh(base::Time now) const;

  base::DictValue ToValue() const;
  // Null if |value| isn't a session with a client and a refresh token.
  static std::optional<Session> FromValue(const base::DictValue& value);
};

// The revocation endpoint in the discovery document; null if it has none.
std::optional<GURL> RevocationEndpoint(std::string_view discovery_json);

// What a Responses error code about the plan means, for the user
// ("subscription_sharing_usage_limit_exceeded": the plan's limit is
// reached); null for any other code. Plan errors never fall back to another
// way of paying.
std::optional<std::string> PlanErrorMessage(std::string_view code);

// The page the loopback redirect shows in the browser.
std::string CallbackPage(bool ok, std::string_view message);

}  // namespace compass::claude::chatgpt

#endif  // COMPASS_BROWSER_CLAUDE_CHATGPT_OAUTH_H_
