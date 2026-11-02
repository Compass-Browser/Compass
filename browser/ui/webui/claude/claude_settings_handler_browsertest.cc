// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/claude/claude_settings_handler.h"

#include <memory>
#include <optional>
#include <string>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/test/test_future.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_prefs.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/http/http_status_code.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude {
namespace {

constexpr char kGoodKey[] = "sk-ant-api03-test-key-0000-1234";

// A stand-in for the Claude API: GET /v1/models answers with two models to
// kGoodKey, and with the API's error to any other.
std::unique_ptr<net::test_server::HttpResponse> FakeApi(
    const net::test_server::HttpRequest& request) {
  if (!base::StartsWith(request.relative_url, "/v1/models")) {
    return nullptr;
  }
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_content_type("application/json");
  const auto key = request.headers.find("x-api-key");
  const auto version = request.headers.find("anthropic-version");
  if (key == request.headers.end() || key->second != kGoodKey ||
      version == request.headers.end() || version->second != kApiVersion) {
    response->set_code(net::HTTP_UNAUTHORIZED);
    response->set_content(R"({"type": "error", "error": {
        "type": "authentication_error", "message": "invalid x-api-key"}})");
    return response;
  }
  response->set_code(net::HTTP_OK);
  response->set_content(R"({"data": [
      {"type": "model", "id": "claude-opus-5-5",
       "display_name": "Claude Opus 5.5"},
      {"type": "model", "id": "claude-haiku-5-5",
       "display_name": "Claude Haiku 5.5"}],
    "has_more": false})");
  return response;
}

class ClaudeSettingsHandlerBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestHandler(
        base::BindRepeating(&FakeApi));
    ASSERT_TRUE(embedded_test_server()->Start());
    handler_ = std::make_unique<ClaudeSettingsHandler>(
        browser()->GetProfile(), remote_.BindNewPipeAndPassReceiver());
  }

  void TearDownOnMainThread() override {
    handler_.reset();
    InProcessBrowserTest::TearDownOnMainThread();
  }

  mojom::SettingsStatePtr State() {
    base::test::TestFuture<mojom::SettingsStatePtr> state;
    remote_->GetState(state.GetCallback());
    return state.Take();
  }

  bool SetKey(const std::string& key) {
    base::test::TestFuture<bool> saved;
    remote_->SetApiKey(key, saved.GetCallback());
    return saved.Get();
  }

  bool SetEndpoint(const std::string& url,
                   mojom::AuthStyle style = mojom::AuthStyle::kApiKey) {
    base::test::TestFuture<bool> ok;
    remote_->SetEndpoint(url, style, ok.GetCallback());
    return ok.Get();
  }

  mojom::ConnectionResultPtr Test() {
    base::test::TestFuture<mojom::ConnectionResultPtr> result;
    remote_->TestConnection(result.GetCallback());
    return result.Take();
  }

  PrefService* prefs() { return browser()->GetProfile()->GetPrefs(); }

  mojo::Remote<mojom::SettingsHandler> remote_;
  std::unique_ptr<ClaudeSettingsHandler> handler_;
};

IN_PROC_BROWSER_TEST_F(ClaudeSettingsHandlerBrowserTest, KeyIsStoredEncrypted) {
  mojom::SettingsStatePtr state = State();
  EXPECT_FALSE(state->has_key);
  EXPECT_EQ(state->model, kDefaultModel);
  EXPECT_FALSE(state->browser_scope);

  ASSERT_TRUE(SetKey(std::string("  ") + kGoodKey + "\n"));
  state = State();
  EXPECT_TRUE(state->has_key);
  EXPECT_EQ(state->key_hint, "sk-ant-…1234");
  // Not in the prefs as typed.
  const std::string stored = prefs()->GetString(prefs::kApiKey);
  EXPECT_FALSE(stored.empty());
  EXPECT_EQ(stored.find("test-key"), std::string::npos);

  base::test::TestFuture<std::optional<std::string>> key;
  ClaudeCredentials::GetApiKey(browser()->GetProfile(), key.GetCallback());
  EXPECT_EQ(key.Get(), kGoodKey);

  // An empty key removes it.
  ASSERT_TRUE(SetKey(""));
  EXPECT_FALSE(State()->has_key);
  EXPECT_TRUE(prefs()->GetString(prefs::kApiKey).empty());
}

IN_PROC_BROWSER_TEST_F(ClaudeSettingsHandlerBrowserTest,
                       TestConnectionListsModels) {
  // No key yet.
  mojom::ConnectionResultPtr result = Test();
  EXPECT_FALSE(result->ok);
  EXPECT_FALSE(result->error.empty());

  ASSERT_TRUE(SetEndpoint(embedded_test_server()->base_url().spec()));
  ASSERT_TRUE(SetKey(kGoodKey));
  result = Test();
  ASSERT_TRUE(result->ok) << result->error;
  ASSERT_EQ(result->models.size(), 2u);
  EXPECT_EQ(result->models[0]->id, "claude-opus-5-5");
  EXPECT_EQ(result->models[1]->display_name, "Claude Haiku 5.5");
}

IN_PROC_BROWSER_TEST_F(ClaudeSettingsHandlerBrowserTest,
                       WrongKeyGivesTheApisMessage) {
  ASSERT_TRUE(SetEndpoint(embedded_test_server()->base_url().spec()));
  ASSERT_TRUE(SetKey("sk-ant-api03-wrong-wrong-wrong"));
  mojom::ConnectionResultPtr result = Test();
  EXPECT_FALSE(result->ok);
  EXPECT_EQ(result->error, "invalid x-api-key");

  // As a bearer token the server doesn't take it either.
  ASSERT_TRUE(SetEndpoint(embedded_test_server()->base_url().spec(),
                          mojom::AuthStyle::kBearer));
  ASSERT_TRUE(SetKey(kGoodKey));
  EXPECT_FALSE(Test()->ok);
}

IN_PROC_BROWSER_TEST_F(ClaudeSettingsHandlerBrowserTest, EndpointRules) {
  // The key isn't sent in the clear to another computer.
  EXPECT_FALSE(SetEndpoint("http://gateway.example.com"));
  EXPECT_FALSE(SetEndpoint("https://gateway.example.com/?key=1"));
  EXPECT_TRUE(SetEndpoint("https://gateway.example.com/anthropic/",
                          mojom::AuthStyle::kBearer));
  mojom::SettingsStatePtr state = State();
  EXPECT_EQ(state->base_url, "https://gateway.example.com/anthropic");
  EXPECT_EQ(state->auth_style, mojom::AuthStyle::kBearer);
  // The Claude API is kept as no URL.
  EXPECT_TRUE(SetEndpoint("https://api.anthropic.com/"));
  EXPECT_EQ(State()->base_url, "");

  remote_->SetModel("claude-haiku-5-5");
  remote_->SetBrowserScope(true);
  state = State();
  EXPECT_EQ(state->model, "claude-haiku-5-5");
  EXPECT_TRUE(state->browser_scope);
}

}  // namespace
}  // namespace compass::claude
