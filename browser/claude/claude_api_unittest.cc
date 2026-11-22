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

// A gateway that refused the cache breakpoints is remembered by its base
// URL, with the level that got through; another URL, and the Claude API,
// don't use it.
TEST(ClaudeApiTest, CacheLevelPerEndpoint) {
  TestingPrefServiceSimple prefs;
  prefs::RegisterProfilePrefs(prefs.registry());
  const GURL gateway("https://gateway.test/anthropic/");
  EXPECT_EQ(CacheLevelFor(prefs, gateway), CacheLevel::kNormal);
  RememberCacheLevel(prefs, gateway, CacheLevel::kNone);
  EXPECT_EQ(CacheLevelFor(prefs, gateway), CacheLevel::kNone);
  RememberCacheLevel(prefs, gateway, CacheLevel::kTopLevelOnly);
  EXPECT_EQ(CacheLevelFor(prefs, gateway), CacheLevel::kTopLevelOnly);
  EXPECT_EQ(CacheLevelFor(prefs, GURL("https://other.test/")),
            CacheLevel::kNormal);
  EXPECT_EQ(CacheLevelFor(prefs, GURL(kDefaultBaseUrl)), CacheLevel::kNormal);
  RememberCacheLevel(prefs, GURL(kDefaultBaseUrl), CacheLevel::kNone);
  EXPECT_TRUE(prefs.GetString(prefs::kCacheLevelUrl).empty());

  EXPECT_EQ(LowerCacheLevel(CacheLevel::kNormal), CacheLevel::kTopLevelOnly);
  EXPECT_EQ(LowerCacheLevel(CacheLevel::kTopLevelOnly), CacheLevel::kNone);
  EXPECT_EQ(LowerCacheLevel(CacheLevel::kNone), std::nullopt);
}

// The refusal, in the API's words or a gateway's.
TEST(ClaudeApiTest, TooManyCacheBreakpoints) {
  EXPECT_TRUE(IsTooManyCacheBreakpoints(
      "A maximum of 4 blocks with cache_control may be provided. Found 5."));
  EXPECT_TRUE(IsTooManyCacheBreakpoints(
      "A MAXIMUM OF 4 BLOCKS WITH CACHE_CONTROL MAY BE PROVIDED"));
  EXPECT_TRUE(IsTooManyCacheBreakpoints(
      R"(upstream 400: {"error":{"message":"A maximum of 4 blocks with )"
      R"(cache_control may be provided. Found 6."}})"));
  EXPECT_FALSE(IsTooManyCacheBreakpoints("Overloaded"));
  EXPECT_FALSE(IsTooManyCacheBreakpoints("cache_control: unknown field"));
  // A 400 body in a gateway's own shape, or plain text, keeps the words.
  EXPECT_TRUE(IsTooManyCacheBreakpoints(DescribeError(
      400, R"({"detail":"A maximum of 4 blocks with cache_control may be )"
           R"(provided. Found 7."})")));
  EXPECT_TRUE(IsTooManyCacheBreakpoints(DescribeError(
      400, "Bad request: maximum of 4 blocks with cache_control exceeded")));
  EXPECT_EQ(DescribeError(400, "<html>nope</html>"),
            "The server answered with HTTP 400.");
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

// docs/design/ai-providers.md: another provider's paths, headers, errors and
// model lists; Anthropic's don't change.
TEST(ClaudeApiTest, ChatGPTPlan) {
  TestingPrefServiceSimple prefs;
  prefs::RegisterProfilePrefs(prefs.registry());
  prefs.SetString(prefs::kProvider, "openai");
  prefs.SetString(prefs::ProviderPref(prefs::kBaseUrl, AIProvider::kOpenAI),
                  "https://gw.example.com/v1");
  EXPECT_FALSE(UsesChatGPT(prefs));
  EXPECT_FALSE(EndpointFromPrefs(prefs).chatgpt);
  // Signed in: OpenAI's own API, whatever gateway was set.
  prefs.SetString(prefs::kChatGPTSession, "c2Vzc2lvbg==");
  EXPECT_TRUE(UsesChatGPT(prefs));
  Endpoint endpoint = EndpointFromPrefs(prefs);
  EXPECT_TRUE(endpoint.chatgpt);
  EXPECT_EQ(EndpointUrl(endpoint.base_url, ChatPath(endpoint.provider)).spec(),
            "https://api.openai.com/v1/responses");
  // Another provider doesn't use it.
  prefs.SetString(prefs::kProvider, "anthropic");
  EXPECT_FALSE(UsesChatGPT(prefs));

  // The plan's model list: listed ones, in its order.
  std::optional<std::vector<ModelInfo>> models = ParseModelList(
      R"({"models":[{"slug":"gpt-6.2-sol","display_name":"GPT-6.2 Sol",)"
      R"("visibility":"list"},{"slug":"hidden","visibility":"hide"},)"
      R"({"slug":"gpt-6-luna","display_name":"GPT-6 Luna",)"
      R"("visibility":"list"}]})",
      AIProvider::kOpenAI);
  ASSERT_TRUE(models);
  ASSERT_EQ(models->size(), 2u);
  EXPECT_EQ((*models)[0].id, "gpt-6.2-sol");
  EXPECT_EQ((*models)[0].display_name, "GPT-6.2 Sol");
  EXPECT_EQ((*models)[1].id, "gpt-6-luna");

  // The plan's errors, by their code.
  EXPECT_NE(DescribeError(
                429,
                R"({"error":{"code":"subscription_sharing_usage_limit_exceeded",)"
                R"("message":"limit"}})",
                AIProvider::kOpenAI)
                .find("usage limit"),
            std::string::npos);
  EXPECT_EQ(DescribeError(401, R"({"detail":"Unauthorized"})",
                          AIProvider::kOpenAI),
            "Unauthorized");
}

TEST(ClaudeApiTest, OtherProviders) {
  Endpoint openai;
  openai.provider = AIProvider::kOpenAI;
  openai.base_url = *ParseBaseUrl("", AIProvider::kOpenAI);
  openai.api_key = "sk-test";
  // OpenAI's own models take tools only through the Responses API.
  EXPECT_EQ(EndpointUrl(openai.base_url, ChatPath(openai.provider)).spec(),
            "https://api.openai.com/v1/responses");
  EXPECT_EQ(ModelsUrl(openai).spec(), "https://api.openai.com/v1/models");
  net::HttpRequestHeaders headers;
  AddRequestHeaders(openai, headers);
  AddBetas(openai, {"a"}, headers);
  EXPECT_EQ(headers.GetHeader("Authorization"), "Bearer sk-test");
  EXPECT_FALSE(headers.HasHeader("anthropic-version"));
  EXPECT_FALSE(headers.HasHeader("anthropic-beta"));
  EXPECT_FALSE(headers.HasHeader("x-api-key"));
  // A key that looks like a subscription token is just a key here.
  openai.api_key = "sk-ant-oat01-abcdef";
  EXPECT_FALSE(UsesOAuthToken(openai));
  EXPECT_FALSE(NeedsTokenRisksAccepted(openai));

  EXPECT_EQ(ParseBaseUrl("", AIProvider::kGemini)->spec(),
            "https://generativelanguage.googleapis.com/v1beta/openai");
  EXPECT_EQ(ChatPath(AIProvider::kGemini), "chat/completions");
  // An OpenAI-compatible server needs a URL.
  EXPECT_FALSE(ParseBaseUrl("", AIProvider::kCompatible));
  EXPECT_EQ(ParseBaseUrl("http://localhost:11434/v1", AIProvider::kCompatible)
                ->spec(),
            "http://localhost:11434/v1");
  // A local server without a key: no Authorization.
  Endpoint local;
  local.provider = AIProvider::kCompatible;
  net::HttpRequestHeaders none;
  AddRequestHeaders(local, none);
  EXPECT_FALSE(none.HasHeader("Authorization"));

  // Errors: Gemini's array, and words for the provider.
  EXPECT_EQ(DescribeError(
                400, R"([{"error":{"code":400,"message":"API key not valid."}}])",
                AIProvider::kGemini),
            "API key not valid.");
  EXPECT_TRUE(DescribeError(529, "", AIProvider::kOpenAI)
                  .starts_with("The server is overloaded"));
  EXPECT_TRUE(DescribeError(529, "").starts_with("Claude is overloaded"));

  // Model lists: OpenAI's chat models only, sorted; Gemini's without models/.
  std::optional<std::vector<ModelInfo>> models = ParseModelList(
      R"({"data":[{"id":"gpt-5-mini"},{"id":"text-embedding-3-large"},)"
      R"({"id":"whisper-1"},{"id":"gpt-5"},{"id":"gpt-image-1"}]})",
      AIProvider::kOpenAI);
  ASSERT_TRUE(models);
  ASSERT_EQ(models->size(), 2u);
  EXPECT_EQ((*models)[0].id, "gpt-5");
  EXPECT_EQ((*models)[1].id, "gpt-5-mini");
  models = ParseModelList(
      R"({"object":"list","data":[{"id":"models/gemini-2.5-flash"}]})",
      AIProvider::kGemini);
  ASSERT_TRUE(models);
  ASSERT_EQ(models->size(), 1u);
  EXPECT_EQ((*models)[0].id, "gemini-2.5-flash");
}

// Settings saved under the names ProviderPref() gave before move to the
// new ones, and what they left in Anthropic's settings goes.
TEST(ClaudeApiTest, MigratesProviderPrefs) {
  TestingPrefServiceSimple prefs;
  prefs::RegisterProfilePrefs(prefs.registry());
  // As the old names left them: OpenAI's values, and Anthropic's key a
  // dictionary holding them.
  prefs.SetString("compass.claude.api_key.openai", "encrypted-openai");
  prefs.SetString("compass.claude.model.openai", "gpt-6.2-sol");
  prefs.SetUserPref(prefs::kApiKey,
                    base::Value(base::DictValue().Set("openai", "x")));
  // A setting the user set under both: the new one stays.
  prefs.SetString("compass.claude.base_url.gemini", "https://old.example/");
  prefs.SetString(prefs::ProviderPref(prefs::kBaseUrl, AIProvider::kGemini),
                  "https://new.example/");
  // Anthropic's own value, untouched.
  prefs.SetString(prefs::kModel, "claude-sonnet-5-5");

  prefs::MigrateProviderPrefs(&prefs);

  EXPECT_EQ(prefs.GetString(
                prefs::ProviderPref(prefs::kApiKey, AIProvider::kOpenAI)),
            "encrypted-openai");
  EXPECT_EQ(prefs.GetString(
                prefs::ProviderPref(prefs::kModel, AIProvider::kOpenAI)),
            "gpt-6.2-sol");
  EXPECT_EQ(prefs.GetString(
                prefs::ProviderPref(prefs::kBaseUrl, AIProvider::kGemini)),
            "https://new.example/");
  EXPECT_FALSE(prefs.HasPrefPath("compass.claude.api_key.openai"));
  EXPECT_FALSE(prefs.HasPrefPath("compass.claude.base_url.gemini"));
  EXPECT_FALSE(prefs.FindPreference(prefs::kApiKey)->HasUserSetting());
  EXPECT_EQ(prefs.GetString(prefs::kApiKey), "");
  EXPECT_EQ(prefs.GetString(prefs::kModel), "claude-sonnet-5-5");

  // Again: nothing more to do.
  prefs::MigrateProviderPrefs(&prefs);
  EXPECT_EQ(prefs.GetString(prefs::kModel), "claude-sonnet-5-5");
}

// Each provider keeps its own key, base URL and models; Anthropic's names
// don't change.
TEST(ClaudeApiTest, ProviderPrefs) {
  TestingPrefServiceSimple prefs;
  prefs::RegisterProfilePrefs(prefs.registry());
  EXPECT_EQ(prefs::CurrentProvider(prefs), AIProvider::kAnthropic);
  EXPECT_EQ(prefs::CurrentPref(prefs, prefs::kModel), prefs::kModel);
  EXPECT_EQ(prefs::ProviderPref(prefs::kTidyTodayModel, AIProvider::kOpenAI),
            "compass.ai_providers.openai.tidy_today_model");
  // Never under Anthropic's own name, which prefs' dotted paths would turn
  // into a dictionary.
  EXPECT_EQ(prefs::ProviderPref(prefs::kApiKey, AIProvider::kGemini),
            "compass.ai_providers.gemini.api_key");

  prefs.SetString(prefs::kBaseUrl, "https://gw.example.com/a");
  prefs.SetString(prefs::kProvider, "gemini");
  EXPECT_EQ(prefs::CurrentProvider(prefs), AIProvider::kGemini);
  EXPECT_EQ(prefs.GetString(prefs::CurrentPref(prefs, prefs::kModel)),
            "gemini-2.5-pro");
  EXPECT_EQ(
      prefs.GetString(prefs::CurrentPref(prefs, prefs::kCommandBarAskModel)),
      "gemini-2.5-flash");
  // Anthropic's gateway isn't Gemini's.
  Endpoint endpoint = EndpointFromPrefs(prefs);
  EXPECT_EQ(endpoint.provider, AIProvider::kGemini);
  EXPECT_EQ(endpoint.base_url.spec(),
            "https://generativelanguage.googleapis.com/v1beta/openai");

  // An OpenAI-compatible server without a URL has none.
  prefs.SetString(prefs::kProvider, "compatible");
  EXPECT_FALSE(EndpointFromPrefs(prefs).base_url.is_valid());
  EXPECT_EQ(prefs.GetString(prefs::CurrentPref(prefs, prefs::kModel)), "");
  prefs.SetString(prefs::CurrentPref(prefs, prefs::kBaseUrl),
                  "http://localhost:11434/v1");
  EXPECT_EQ(EndpointFromPrefs(prefs).base_url.spec(),
            "http://localhost:11434/v1");

  // Anything else is Anthropic.
  prefs.SetString(prefs::kProvider, "nope");
  EXPECT_EQ(EndpointFromPrefs(prefs).base_url.spec(),
            "https://gw.example.com/a");
}

TEST(ClaudeApiTest, KeyHint) {
  EXPECT_EQ(KeyHint("sk-ant-api03-abcdefghijklmnop-WXYZ"), "sk-ant-…WXYZ");
  EXPECT_EQ(KeyHint("short"), "…");
}

}  // namespace
}  // namespace compass::claude
