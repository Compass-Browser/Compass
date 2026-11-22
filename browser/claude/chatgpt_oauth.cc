// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/chatgpt_oauth.h"

#include <array>
#include <utility>
#include <vector>

#include "base/base64url.h"
#include "base/json/json_reader.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/uuid.h"
#include "crypto/random.h"
#include "crypto/sha2.h"
#include "net/base/url_util.h"

namespace compass::claude::chatgpt {

namespace {

// The ID token's expiry and issue time may be off by this much.
constexpr base::TimeDelta kClockLeeway = base::Seconds(30);

std::string Base64Url(base::span<const uint8_t> bytes) {
  std::string out;
  base::Base64UrlEncode(bytes, base::Base64UrlEncodePolicy::OMIT_PADDING,
                        &out);
  return out;
}

// "a=b&c=d", each value escaped.
std::string FormEncode(
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        fields) {
  std::vector<std::string> parts;
  for (const auto& [name, value] : fields) {
    parts.push_back(base::StrCat(
        {name, "=", base::EscapeQueryParamValue(value, /*use_plus=*/true)}));
  }
  return base::JoinString(parts, "&");
}

std::string StringOr(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

// Refresh errors after which the refresh token is no good.
bool EndsSignIn(std::string_view code) {
  for (std::string_view terminal :
       {"invalid_grant", "invalid_refresh_token", "token_expired",
        "refresh_token_expired", "refresh_token_invalidated",
        "refresh_token_reused"}) {
    if (code == terminal) {
      return true;
    }
  }
  return false;
}

// A JWT's payload, unverified.
std::optional<base::DictValue> JwtPayload(std::string_view jwt) {
  std::vector<std::string_view> parts = base::SplitStringPiece(
      jwt, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (parts.size() != 3) {
    return std::nullopt;
  }
  std::string json;
  if (!base::Base64UrlDecode(parts[1],
                             base::Base64UrlDecodePolicy::IGNORE_PADDING,
                             &json)) {
    return std::nullopt;
  }
  return base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
}

// "aud" is a string or a list of them.
bool HasAudience(const base::DictValue& claims, std::string_view audience) {
  if (const std::string* aud = claims.FindString("aud")) {
    return *aud == audience;
  }
  if (const base::ListValue* list = claims.FindList("aud")) {
    for (const base::Value& each : *list) {
      if (each.is_string() && each.GetString() == audience) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

AuthorizeParams::AuthorizeParams() = default;
AuthorizeParams::AuthorizeParams(const AuthorizeParams&) = default;
AuthorizeParams& AuthorizeParams::operator=(const AuthorizeParams&) = default;
AuthorizeParams::~AuthorizeParams() = default;

CallbackResult::CallbackResult() = default;
CallbackResult::CallbackResult(const CallbackResult&) = default;
CallbackResult& CallbackResult::operator=(const CallbackResult&) = default;
CallbackResult::~CallbackResult() = default;

TokenReply::TokenReply() = default;
TokenReply::TokenReply(const TokenReply&) = default;
TokenReply& TokenReply::operator=(const TokenReply&) = default;
TokenReply::~TokenReply() = default;

Session::Session() = default;
Session::Session(const Session&) = default;
Session& Session::operator=(const Session&) = default;
Session::~Session() = default;

std::string RandomToken() {
  return Base64Url(crypto::RandBytesAsArray<32>());
}

std::string CodeChallenge(std::string_view verifier) {
  return Base64Url(crypto::SHA256Hash(base::as_byte_span(verifier)));
}

std::string NewHostId() {
  return base::StrCat(
      {"urn:uuid:", base::Uuid::GenerateRandomV4().AsLowercaseString()});
}

std::string RedirectUri(int port) {
  return base::StrCat(
      {"http://127.0.0.1:", base::NumberToString(port), kCallbackPath});
}

GURL AuthorizeUrl(const AuthorizeParams& params) {
  const bool first = params.client_id.empty();
  std::vector<std::pair<std::string_view, std::string_view>> query = {
      {"response_type", "code"},
      {"client_id", first ? std::string_view(kDynamicClientId)
                          : std::string_view(params.client_id)},
      {"redirect_uri", params.redirect_uri},
      {"scope", kScopes},
      {"resource", kResource},
      {"state", params.state},
      {"nonce", params.nonce},
      {"code_challenge", params.code_challenge},
      {"code_challenge_method", "S256"},
      {"ext_agent_host_id", params.host_id},
  };
  if (first) {
    // Only when registering: the name the user approves (and may edit).
    query.push_back({"agent_name_hint", kAgentName});
  } else {
    if (!params.id_token_hint.empty()) {
      query.push_back({"id_token_hint", params.id_token_hint});
    }
    if (!params.login_hint.empty()) {
      query.push_back({"login_hint", params.login_hint});
    }
  }
  std::vector<std::string> parts;
  for (const auto& [name, value] : query) {
    parts.push_back(base::StrCat(
        {name, "=", base::EscapeQueryParamValue(value, /*use_plus=*/false)}));
  }
  return GURL(base::StrCat({kAuthorizeUrl, "?", base::JoinString(parts, "&")}));
}

CallbackResult ParseCallback(const GURL& url) {
  CallbackResult result;
  for (net::QueryIterator it(url); !it.IsAtEnd(); it.Advance()) {
    const std::string_view key = it.GetKey();
    const std::string& value = it.GetUnescapedValue();
    if (key == "code") {
      result.code = value;
    } else if (key == "state") {
      result.state = value;
    } else if (key == "client_id") {
      result.client_id = value;
    } else if (key == "scope") {
      result.scope = value;
    } else if (key == "error") {
      result.error = value;
    } else if (key == "error_description") {
      result.error_description = value;
    }
  }
  return result;
}

base::expected<std::string, std::string> ClientForCallback(
    std::string_view saved_client_id,
    const CallbackResult& callback) {
  if (saved_client_id.empty()) {
    if (callback.client_id.empty() ||
        callback.client_id == kDynamicClientId) {
      return base::unexpected(
          "ChatGPT didn't finish registering Compass. Try signing in again.");
    }
    return base::ok(callback.client_id);
  }
  // One registration per account and device: never replaced by another.
  if (!callback.client_id.empty() && callback.client_id != saved_client_id) {
    return base::unexpected(
        "ChatGPT answered for another registration than Compass's. Try "
        "signing in again.");
  }
  return base::ok(std::string(saved_client_id));
}

std::string CodeExchangeBody(std::string_view client_id,
                             std::string_view code,
                             std::string_view verifier,
                             std::string_view redirect_uri) {
  return FormEncode({{"grant_type", "authorization_code"},
                     {"client_id", client_id},
                     {"code", code},
                     {"code_verifier", verifier},
                     {"redirect_uri", redirect_uri},
                     {"resource", kResource}});
}

std::string RefreshBody(std::string_view client_id,
                        std::string_view refresh_token) {
  // No scope: the grant stays as it was.
  return FormEncode({{"grant_type", "refresh_token"},
                     {"client_id", client_id},
                     {"refresh_token", refresh_token},
                     {"resource", kResource}});
}

std::string RevokeBody(std::string_view client_id,
                       std::string_view refresh_token) {
  return FormEncode({{"token", refresh_token},
                     {"token_type_hint", "refresh_token"},
                     {"client_id", client_id}});
}

base::expected<TokenReply, TokenError> ParseTokenReply(int http_status,
                                                       std::string_view body) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC);
  if (http_status == 200 && dict) {
    TokenReply reply;
    reply.access_token = StringOr(*dict, "access_token");
    reply.refresh_token = StringOr(*dict, "refresh_token");
    reply.id_token = StringOr(*dict, "id_token");
    reply.scope = StringOr(*dict, "scope");
    reply.expires_in = base::Seconds(dict->FindInt("expires_in").value_or(0));
    if (!reply.access_token.empty()) {
      return reply;
    }
  }
  TokenError error;
  if (http_status == 0) {
    error.message = "Couldn't reach ChatGPT. Check the connection.";
    return base::unexpected(std::move(error));
  }
  if (dict) {
    error.code = StringOr(*dict, "error");
    if (const base::DictValue* nested = dict->FindDict("error")) {
      // Some replies wrap it: {"error": {"code": …, "message": …}}.
      error.code = StringOr(*nested, "code");
      error.message = StringOr(*nested, "message");
    }
    if (error.message.empty()) {
      error.message = StringOr(*dict, "error_description");
    }
  }
  if (EndsSignIn(error.code)) {
    error.needs_sign_in = true;
    error.message = "Your ChatGPT sign-in has ended. Sign in again.";
  } else if (error.code == "invalid_client") {
    error.message =
        "ChatGPT doesn't know Compass's registration any more. Sign in "
        "again.";
    error.needs_sign_in = true;
  } else if (http_status >= 500) {
    error.message = "ChatGPT's sign-in is unavailable right now. Try again "
                    "in a moment.";
  } else if (error.message.empty()) {
    error.message = base::StrCat(
        {"ChatGPT refused the sign-in (HTTP ",
         base::NumberToString(http_status), ")."});
  }
  return base::unexpected(std::move(error));
}

bool HasPlanScope(std::string_view scope) {
  for (std::string_view each : base::SplitStringPiece(
           scope, " ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (each == kPlanScope) {
      return true;
    }
  }
  return false;
}

base::expected<IdTokenClaims, std::string> CheckIdToken(
    std::string_view id_token,
    std::string_view client_id,
    std::string_view nonce,
    base::Time now) {
  std::optional<base::DictValue> claims = JwtPayload(id_token);
  if (!claims) {
    return base::unexpected("ChatGPT's sign-in reply couldn't be read.");
  }
  if (StringOr(*claims, "iss") != kIssuer ||
      !HasAudience(*claims, client_id) || StringOr(*claims, "nonce") != nonce) {
    return base::unexpected(
        "ChatGPT's sign-in reply wasn't for this sign-in. Try again.");
  }
  const std::optional<double> exp = claims->FindDouble("exp");
  if (!exp ||
      base::Time::FromSecondsSinceUnixEpoch(*exp) + kClockLeeway < now) {
    return base::unexpected(
        "ChatGPT's sign-in reply has expired. Check this device's clock, "
        "then try again.");
  }
  IdTokenClaims result;
  result.subject = StringOr(*claims, "sub");
  result.email = StringOr(*claims, "email");
  if (result.subject.empty()) {
    return base::unexpected("ChatGPT's sign-in reply named no account.");
  }
  return result;
}

void Session::Apply(const TokenReply& reply, base::Time now) {
  access_token = reply.access_token;
  if (!reply.refresh_token.empty()) {
    refresh_token = reply.refresh_token;
  }
  if (!reply.id_token.empty()) {
    id_token = reply.id_token;
  }
  // Without expires_in, an hour (what the docs give).
  expires_at = now + (reply.expires_in.is_positive() ? reply.expires_in
                                                     : base::Hours(1));
  if (!reply.scope.empty()) {
    scope = reply.scope;
  }
}

bool Session::IsFresh(base::Time now) const {
  return !access_token.empty() && now + kRefreshMargin < expires_at;
}

base::DictValue Session::ToValue() const {
  return base::DictValue()
      .Set("client_id", client_id)
      .Set("access_token", access_token)
      .Set("refresh_token", refresh_token)
      .Set("id_token", id_token)
      .Set("expires_at", expires_at.InSecondsFSinceUnixEpoch())
      .Set("scope", scope)
      .Set("email", email)
      .Set("subject", subject);
}

// static
std::optional<Session> Session::FromValue(const base::DictValue& value) {
  Session session;
  session.client_id = StringOr(value, "client_id");
  session.access_token = StringOr(value, "access_token");
  session.refresh_token = StringOr(value, "refresh_token");
  session.id_token = StringOr(value, "id_token");
  session.expires_at = base::Time::FromSecondsSinceUnixEpoch(
      value.FindDouble("expires_at").value_or(0));
  session.scope = StringOr(value, "scope");
  session.email = StringOr(value, "email");
  session.subject = StringOr(value, "subject");
  if (session.client_id.empty() || session.refresh_token.empty()) {
    return std::nullopt;
  }
  return session;
}

std::optional<GURL> RevocationEndpoint(std::string_view discovery_json) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(discovery_json, base::JSON_PARSE_RFC);
  if (!dict) {
    return std::nullopt;
  }
  GURL url(StringOr(*dict, "revocation_endpoint"));
  // Only OpenAI's own, over TLS: the refresh token goes there.
  if (!url.is_valid() || !url.SchemeIs("https") ||
      url.host() != GURL(kIssuer).host()) {
    return std::nullopt;
  }
  return url;
}

std::optional<std::string> PlanErrorMessage(std::string_view code) {
  if (code == "subscription_sharing_usage_limit_exceeded") {
    return base::StrCat(
        {"You've reached your ChatGPT plan's usage limit (or the limit you "
         "set for Compass). See ",
         kUsageUrl, ", or use an API key instead."});
  }
  if (code == "subscription_sharing_user_not_eligible") {
    return std::string(
        "Your ChatGPT plan can't be used in other apps. Use an OpenAI API "
        "key instead.");
  }
  if (code == "subscription_sharing_usage_unavailable" ||
      code == "subscription_sharing_user_unavailable") {
    return std::string(
        "ChatGPT plan usage is unavailable right now. Try again in a "
        "moment.");
  }
  if (code == "subscription_sharing_invalid_user") {
    return std::string(
        "ChatGPT didn't accept your sign-in. If you disconnected Compass in "
        "ChatGPT's settings, sign in again.");
  }
  if (code == "subscription_sharing_unsupported_capability") {
    return std::string(
        "This request uses something your ChatGPT plan doesn't support "
        "here. Try another model, or an API key.");
  }
  if (code == "subscription_sharing_route_not_supported" ||
      code == "chatpass_v2_scope_not_authorized" ||
      code == "chatpass_v2_invalid_authorization_context") {
    return base::StrCat({"ChatGPT refused the request (", code,
                         "). Try signing in with ChatGPT again."});
  }
  return std::nullopt;
}

std::string CallbackPage(bool ok, std::string_view message) {
  const std::string text = base::EscapeForHTML(message);
  return base::StrCat(
      {"<!doctype html><html><head><meta charset=\"utf-8\">"
       "<meta name=\"viewport\" content=\"width=device-width\">"
       "<title>",
       ok ? "Signed in to ChatGPT" : "ChatGPT sign-in failed",
       "</title><style>"
       ":root{color-scheme:light dark}"
       "body{font:16px/1.5 system-ui,sans-serif;max-width:32em;"
       "margin:20vh auto;padding:0 16px;text-align:center}"
       "h1{font-size:22px;font-weight:600}"
       "</style></head><body><h1>",
       ok ? "You're signed in" : "Sign-in didn't work", "</h1><p>", text,
       "</p></body></html>"});
}

}  // namespace compass::claude::chatgpt
