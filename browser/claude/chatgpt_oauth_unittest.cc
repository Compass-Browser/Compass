// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/chatgpt_oauth.h"

#include <map>
#include <string>

#include "base/base64url.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "net/base/url_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude::chatgpt {
namespace {

std::map<std::string, std::string> Query(const GURL& url) {
  std::map<std::string, std::string> query;
  for (net::QueryIterator it(url); !it.IsAtEnd(); it.Advance()) {
    query[std::string(it.GetKey())] = it.GetUnescapedValue();
  }
  return query;
}

// An unsigned JWT with |claims| as its payload.
std::string Jwt(const base::DictValue& claims) {
  std::string payload;
  base::Base64UrlEncode(*base::WriteJson(claims),
                        base::Base64UrlEncodePolicy::OMIT_PADDING, &payload);
  return base::StrCat({"eyJhbGciOiJSUzI1NiJ9.", payload, ".c2ln"});
}

base::DictValue Claims(base::Time now) {
  return base::DictValue()
      .Set("iss", kIssuer)
      .Set("aud", "oaiapp_123")
      .Set("nonce", "n0nce")
      .Set("sub", "user-1")
      .Set("email", "a@example.com")
      .Set("iat", now.InSecondsFSinceUnixEpoch())
      .Set("exp", (now + base::Hours(1)).InSecondsFSinceUnixEpoch());
}

TEST(ChatGPTOAuthTest, Pkce) {
  // RFC 7636, appendix B.
  EXPECT_EQ(CodeChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
            "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
  const std::string token = RandomToken();
  EXPECT_EQ(token.size(), 43u);
  EXPECT_NE(token, RandomToken());
  EXPECT_TRUE(NewHostId().starts_with("urn:uuid:"));
  EXPECT_EQ(RedirectUri(1455), "http://127.0.0.1:1455/auth/callback");
}

TEST(ChatGPTOAuthTest, AuthorizeUrlFirstSignIn) {
  AuthorizeParams params;
  params.redirect_uri = RedirectUri(1455);
  params.state = "st";
  params.nonce = "nn";
  params.code_challenge = "cc";
  params.host_id = "urn:uuid:abc";
  params.login_hint = "a@example.com";
  const GURL url = AuthorizeUrl(params);
  EXPECT_EQ(url.GetWithEmptyPath().spec(), "https://auth.openai.com/");
  EXPECT_EQ(url.path(), "/api/accounts/authorize");
  std::map<std::string, std::string> query = Query(url);
  EXPECT_EQ(query["client_id"], "dynamic_agent_client");
  EXPECT_EQ(query["response_type"], "code");
  EXPECT_EQ(query["redirect_uri"], "http://127.0.0.1:1455/auth/callback");
  EXPECT_EQ(query["scope"], kScopes);
  EXPECT_EQ(query["resource"], "https://api.openai.com/v1");
  EXPECT_EQ(query["code_challenge_method"], "S256");
  EXPECT_EQ(query["agent_name_hint"], "Compass");
  EXPECT_EQ(query["ext_agent_host_id"], "urn:uuid:abc");
  // Hints only when reauthorizing.
  EXPECT_FALSE(query.contains("login_hint"));
  // Spaces as %20, not +.
  EXPECT_NE(url.spec().find("scope=openid%20profile"), std::string::npos);
}

TEST(ChatGPTOAuthTest, AuthorizeUrlLaterSignIn) {
  AuthorizeParams params;
  params.client_id = "oaiapp_123";
  params.redirect_uri = RedirectUri(5000);
  params.login_hint = "a@example.com";
  std::map<std::string, std::string> query = Query(AuthorizeUrl(params));
  EXPECT_EQ(query["client_id"], "oaiapp_123");
  EXPECT_FALSE(query.contains("agent_name_hint"));
  EXPECT_EQ(query["login_hint"], "a@example.com");
  EXPECT_FALSE(query.contains("id_token_hint"));
}

TEST(ChatGPTOAuthTest, Callback) {
  CallbackResult result = ParseCallback(GURL(
      "http://127.0.0.1:1455/auth/callback?code=c0de&scope=openid+"
      "chatgpt.tokens.use.direct&state=st&client_id=oaiapp_1"));
  EXPECT_EQ(result.code, "c0de");
  EXPECT_EQ(result.state, "st");
  EXPECT_EQ(result.client_id, "oaiapp_1");
  EXPECT_TRUE(HasPlanScope(result.scope));
  EXPECT_FALSE(HasPlanScope("openid chatgpt.tokens"));

  result = ParseCallback(GURL(
      "http://127.0.0.1:1455/auth/callback?error=access_denied&state=st"));
  EXPECT_EQ(result.error, "access_denied");
  EXPECT_TRUE(result.code.empty());
}

TEST(ChatGPTOAuthTest, ClientForCallback) {
  CallbackResult callback;
  // A first sign-in must come back registered.
  EXPECT_FALSE(ClientForCallback("", callback).has_value());
  callback.client_id = "dynamic_agent_client";
  EXPECT_FALSE(ClientForCallback("", callback).has_value());
  callback.client_id = "oaiapp_1";
  EXPECT_EQ(ClientForCallback("", callback).value(), "oaiapp_1");
  // Later: the saved one, never replaced.
  EXPECT_EQ(ClientForCallback("oaiapp_1", callback).value(), "oaiapp_1");
  EXPECT_FALSE(ClientForCallback("oaiapp_2", callback).has_value());
  callback.client_id.clear();
  EXPECT_EQ(ClientForCallback("oaiapp_2", callback).value(), "oaiapp_2");
}

TEST(ChatGPTOAuthTest, Bodies) {
  EXPECT_EQ(CodeExchangeBody("oaiapp_1", "c/d", "v", "http://127.0.0.1:1/x"),
            "grant_type=authorization_code&client_id=oaiapp_1&code=c%2Fd&"
            "code_verifier=v&redirect_uri=http%3A%2F%2F127.0.0.1%3A1%2Fx&"
            "resource=https%3A%2F%2Fapi.openai.com%2Fv1");
  EXPECT_EQ(RefreshBody("oaiapp_1", "rt"),
            "grant_type=refresh_token&client_id=oaiapp_1&refresh_token=rt&"
            "resource=https%3A%2F%2Fapi.openai.com%2Fv1");
  EXPECT_EQ(RevokeBody("oaiapp_1", "rt"),
            "token=rt&token_type_hint=refresh_token&client_id=oaiapp_1");
}

TEST(ChatGPTOAuthTest, TokenReply) {
  auto reply = ParseTokenReply(
      200,
      R"({"access_token":"at","refresh_token":"rt","id_token":"it",)"
      R"("token_type":"Bearer","expires_in":3600,"scope":"openid"})");
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->access_token, "at");
  EXPECT_EQ(reply->refresh_token, "rt");
  EXPECT_EQ(reply->expires_in, base::Hours(1));

  auto error = ParseTokenReply(400, R"({"error":"refresh_token_reused"})");
  ASSERT_FALSE(error.has_value());
  EXPECT_TRUE(error.error().needs_sign_in);
  EXPECT_EQ(error.error().code, "refresh_token_reused");

  // A network or server failure keeps the sign-in.
  EXPECT_FALSE(ParseTokenReply(0, "").error().needs_sign_in);
  EXPECT_FALSE(ParseTokenReply(503, "oops").error().needs_sign_in);
  EXPECT_FALSE(ParseTokenReply(429, "{}").error().needs_sign_in);
  // A 200 without a token isn't one.
  EXPECT_FALSE(ParseTokenReply(200, "{}").has_value());
}

TEST(ChatGPTOAuthTest, IdToken) {
  const base::Time now = base::Time::Now();
  auto claims = CheckIdToken(Jwt(Claims(now)), "oaiapp_123", "n0nce", now);
  ASSERT_TRUE(claims.has_value());
  EXPECT_EQ(claims->subject, "user-1");
  EXPECT_EQ(claims->email, "a@example.com");

  // An audience list.
  base::DictValue listed = Claims(now);
  listed.Set("aud", base::ListValue().Append("x").Append("oaiapp_123"));
  EXPECT_TRUE(CheckIdToken(Jwt(listed), "oaiapp_123", "n0nce", now)
                  .has_value());

  EXPECT_FALSE(CheckIdToken(Jwt(Claims(now)), "oaiapp_9", "n0nce", now)
                   .has_value());
  EXPECT_FALSE(CheckIdToken(Jwt(Claims(now)), "oaiapp_123", "other", now)
                   .has_value());
  EXPECT_FALSE(CheckIdToken(Jwt(Claims(now)), "oaiapp_123", "n0nce",
                            now + base::Hours(2))
                   .has_value());
  base::DictValue issuer = Claims(now);
  issuer.Set("iss", "https://evil.example");
  EXPECT_FALSE(
      CheckIdToken(Jwt(issuer), "oaiapp_123", "n0nce", now).has_value());
  EXPECT_FALSE(CheckIdToken("not.a-jwt", "oaiapp_123", "n0nce", now)
                   .has_value());
}

TEST(ChatGPTOAuthTest, Session) {
  const base::Time now = base::Time::Now();
  Session session;
  session.client_id = "oaiapp_1";
  TokenReply reply;
  reply.access_token = "at1";
  reply.refresh_token = "rt1";
  reply.expires_in = base::Hours(1);
  reply.scope = kScopes;
  session.Apply(reply, now);
  EXPECT_TRUE(session.IsFresh(now));
  EXPECT_FALSE(session.IsFresh(now + base::Minutes(59)));

  // A refresh without a new refresh token keeps the old one.
  reply.access_token = "at2";
  reply.refresh_token.clear();
  session.Apply(reply, now);
  EXPECT_EQ(session.access_token, "at2");
  EXPECT_EQ(session.refresh_token, "rt1");

  std::optional<Session> back = Session::FromValue(session.ToValue());
  ASSERT_TRUE(back);
  EXPECT_EQ(back->refresh_token, "rt1");
  EXPECT_EQ(back->client_id, "oaiapp_1");
  EXPECT_EQ(back->expires_at.InSecondsFSinceUnixEpoch(),
            session.expires_at.InSecondsFSinceUnixEpoch());
  EXPECT_FALSE(Session::FromValue(base::DictValue()));
}

TEST(ChatGPTOAuthTest, RevocationEndpoint) {
  EXPECT_EQ(RevocationEndpoint(
                R"({"revocation_endpoint":"https://auth.openai.com/oauth/revoke"})")
                ->spec(),
            "https://auth.openai.com/oauth/revoke");
  // The refresh token only goes to OpenAI, over TLS.
  EXPECT_FALSE(RevocationEndpoint(
      R"({"revocation_endpoint":"https://evil.example/revoke"})"));
  EXPECT_FALSE(RevocationEndpoint(
      R"({"revocation_endpoint":"http://auth.openai.com/revoke"})"));
  EXPECT_FALSE(RevocationEndpoint("{}"));
}

TEST(ChatGPTOAuthTest, PlanErrors) {
  EXPECT_NE(PlanErrorMessage("subscription_sharing_usage_limit_exceeded")
                ->find(kUsageUrl),
            std::string::npos);
  EXPECT_TRUE(PlanErrorMessage("subscription_sharing_user_not_eligible"));
  EXPECT_FALSE(PlanErrorMessage("rate_limit_exceeded"));
}

TEST(ChatGPTOAuthTest, CallbackPageEscapes) {
  EXPECT_EQ(CallbackPage(false, "<b>").find("<b>"), std::string::npos);
}

}  // namespace
}  // namespace compass::claude::chatgpt
