// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_api.h"

#include <optional>
#include <string>

#include "compass/browser/claude/claude_prefs.h"
#include "components/prefs/testing_pref_service.h"
#include "net/http/http_request_headers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude {
namespace {

std::string Parsed(std::string_view text) {
  std::optional<GURL> url = ParseBaseUrl(text);
  return url ? url->spec() : "invalid";
}

TEST(ClaudeApiTest, ParseBaseUrl) {
  EXPECT_EQ(Parsed(""), "https://api.anthropic.com/");
  EXPECT_EQ(Parsed("  https://gw.example.com/anthropic/ "),
            "https://gw.example.com/anthropic");
  EXPECT_EQ(Parsed("https://gw.example.com"), "https://gw.example.com/");
  // The key isn't sent in the clear, except to this computer.
  EXPECT_EQ(Parsed("http://gw.example.com"), "invalid");
  EXPECT_EQ(Parsed("http://127.0.0.1:8080/"), "http://127.0.0.1:8080/");
  EXPECT_EQ(Parsed("http://localhost:4000/v"), "http://localhost:4000/v");
  EXPECT_EQ(Parsed("https://gw.example.com/?a=b"), "invalid");
  EXPECT_EQ(Parsed("https://user:pw@gw.example.com"), "invalid");
  EXPECT_EQ(Parsed("not a url"), "invalid");
}

TEST(ClaudeApiTest, EndpointUrl) {
  EXPECT_EQ(EndpointUrl(GURL("https://api.anthropic.com/"), "v1/messages")
                .spec(),
            "https://api.anthropic.com/v1/messages");
  EXPECT_EQ(
      EndpointUrl(GURL("https://gw.example.com/anthropic"), "v1/models").spec(),
      "https://gw.example.com/anthropic/v1/models");
}

TEST(ClaudeApiTest, Headers) {
  Endpoint endpoint;
  endpoint.api_key = "sk-ant-test";
  net::HttpRequestHeaders headers;
  AddRequestHeaders(endpoint, headers);
  EXPECT_EQ(headers.GetHeader("x-api-key"), "sk-ant-test");
  EXPECT_EQ(headers.GetHeader("anthropic-version"), kApiVersion);
  EXPECT_FALSE(headers.HasHeader("Authorization"));

  endpoint.auth_style = AuthStyle::kBearer;
  net::HttpRequestHeaders bearer;
  AddRequestHeaders(endpoint, bearer);
  EXPECT_EQ(bearer.GetHeader("Authorization"), "Bearer sk-ant-test");
  EXPECT_FALSE(bearer.HasHeader("x-api-key"));
}

TEST(ClaudeApiTest, OAuthTokenFromSetupToken) {
  EXPECT_TRUE(IsOAuthToken("sk-ant-oat01-abcdef"));
  EXPECT_TRUE(IsOAuthToken("  sk-ant-oat01-abcdef\n"));
  EXPECT_FALSE(IsOAuthToken("sk-ant-api03-abcdef"));
  EXPECT_FALSE(IsOAuthToken(""));

  // Refused until the user accepts the risks.
  Endpoint endpoint;
  endpoint.api_key = "sk-ant-oat01-abcdef";
  EXPECT_TRUE(NeedsTokenRisksAccepted(endpoint));
  EXPECT_FALSE(UsesOAuthToken(endpoint));
  net::HttpRequestHeaders refused;
  AddBetas(endpoint, {}, refused);
  EXPECT_FALSE(refused.HasHeader("anthropic-beta"));

  // Accepted: Bearer even with the x-api-key style, and its beta.
  endpoint.allow_oauth_token = true;
  EXPECT_FALSE(NeedsTokenRisksAccepted(endpoint));
  EXPECT_TRUE(UsesOAuthToken(endpoint));
  net::HttpRequestHeaders headers;
  AddRequestHeaders(endpoint, headers);
  EXPECT_EQ(headers.GetHeader("Authorization"), "Bearer sk-ant-oat01-abcdef");
  EXPECT_FALSE(headers.HasHeader("x-api-key"));
  AddBetas(endpoint, {"a"}, headers);
  EXPECT_EQ(headers.GetHeader("anthropic-beta"),
            std::string("a,") + kOAuthBeta);

  net::HttpRequestHeaders only_oauth;
  AddBetas(endpoint, {}, only_oauth);
  EXPECT_EQ(only_oauth.GetHeader("anthropic-beta"), kOAuthBeta);

  // An API key brings no beta of its own, and needs nothing accepted.
  endpoint.api_key = "sk-ant-api03-abcdef";
  EXPECT_FALSE(NeedsTokenRisksAccepted(endpoint));
  net::HttpRequestHeaders api_key;
  AddBetas(endpoint, {}, api_key);
  EXPECT_FALSE(api_key.HasHeader("anthropic-beta"));
  AddBetas(endpoint, {"a"}, api_key);
  EXPECT_EQ(api_key.GetHeader("anthropic-beta"), "a");
}

TEST(ClaudeApiTest, EndpointFromPrefs) {
  TestingPrefServiceSimple prefs;
  prefs::RegisterProfilePrefs(prefs.registry());
  Endpoint endpoint = EndpointFromPrefs(prefs);
  EXPECT_EQ(endpoint.base_url.spec(), "https://api.anthropic.com/");
  EXPECT_EQ(endpoint.auth_style, AuthStyle::kApiKey);
  EXPECT_TRUE(endpoint.api_key.empty());
  EXPECT_EQ(prefs.GetString(prefs::kModel), kDefaultModel);

  prefs.SetString(prefs::kBaseUrl, "https://gw.example.com/a");
  prefs.SetInteger(prefs::kAuthStyle, static_cast<int>(AuthStyle::kBearer));
  endpoint = EndpointFromPrefs(prefs);
  EXPECT_EQ(endpoint.base_url.spec(), "https://gw.example.com/a");
  EXPECT_EQ(endpoint.auth_style, AuthStyle::kBearer);
  EXPECT_FALSE(endpoint.allow_oauth_token);

  prefs.SetBoolean(prefs::kAcceptTokenRisks, true);
  EXPECT_TRUE(EndpointFromPrefs(prefs).allow_oauth_token);
}

TEST(ClaudeApiTest, ParseModelList) {
  std::optional<std::vector<ModelInfo>> models = ParseModelList(R"({
    "data": [
      {"type": "model", "id": "claude-opus-5-5",
       "display_name": "Claude Opus 5.5"},
      {"type": "model", "id": "claude-haiku-5-5"},
      {"type": "model"}
    ],
    "has_more": false
  })");
  ASSERT_TRUE(models);
  ASSERT_EQ(models->size(), 2u);
  EXPECT_EQ((*models)[0].id, "claude-opus-5-5");
  EXPECT_EQ((*models)[0].display_name, "Claude Opus 5.5");
  // No name: its id.
  EXPECT_EQ((*models)[1].display_name, "claude-haiku-5-5");
  EXPECT_FALSE(ParseModelList("<html>"));
  EXPECT_FALSE(ParseModelList(R"({"models": []})"));
}

TEST(ClaudeApiTest, DescribeError) {
  EXPECT_EQ(DescribeError(401, R"({"type": "error", "error": {
              "type": "authentication_error",
              "message": "invalid x-api-key"}})"),
            "invalid x-api-key");
  EXPECT_NE(DescribeError(401, "").find("401"), std::string::npos);
  EXPECT_NE(DescribeError(502, "<html>").find("502"), std::string::npos);
  EXPECT_NE(DescribeError(0, "").find("reach"), std::string::npos);
}

TEST(ClaudeApiTest, KeyHint) {
  EXPECT_EQ(KeyHint("sk-ant-api03-abcdefghijklmnop-WXYZ"), "sk-ant-…WXYZ");
  EXPECT_EQ(KeyHint("short"), "…");
}

}  // namespace
}  // namespace compass::claude
