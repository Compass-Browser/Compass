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
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/claude_code.h"
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
// kGoodKey, and with the API's error to any other. Under /compat, an
// OpenAI-compatible server that takes no key: GET /compat/models lists one.
std::unique_ptr<net::test_server::HttpResponse> FakeApi(
    const net::test_server::HttpRequest& request) {
  if (request.relative_url == "/compat/models") {
    auto response = std::make_unique<net::test_server::BasicHttpResponse>();
    response->set_content_type("application/json");
    response->set_code(net::HTTP_OK);
    response->set_content(
        R"({"object": "list", "data": [{"id": "llama-4", "object": "model"}]})");
    return response;
  }
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

// "Let Claude use developer tools": off by default, local to the profile.
IN_PROC_BROWSER_TEST_F(ClaudeSettingsHandlerBrowserTest, DevToolsSetting) {
  EXPECT_FALSE(State()->devtools);
  EXPECT_FALSE(prefs()->GetBoolean(prefs::kDevTools));
  remote_->SetDevTools(true);
  // Mojo keeps the order: the state read next has the change.
  EXPECT_TRUE(State()->devtools);
  EXPECT_TRUE(prefs()->GetBoolean(prefs::kDevTools));
  remote_->SetDevTools(false);
  EXPECT_FALSE(State()->devtools);
}

// Claude in the sidebar (docs/design/ai-sidebar.md): "Name Link Folders",
// "Tidy Today" and "Shorter Tab Titles" off, and their models Claude Haiku,
// by default; each its own.
IN_PROC_BROWSER_TEST_F(ClaudeSettingsHandlerBrowserTest, SidebarAiSettings) {
  mojom::SettingsStatePtr state = State();
  EXPECT_TRUE(state->sidebar_supported);
  EXPECT_FALSE(state->name_link_folders);
  EXPECT_EQ(state->name_link_folders_model, "claude-haiku-5-5");
  remote_->SetNameLinkFolders(true);
  remote_->SetNameLinkFoldersModel(" claude-sonnet-5-5 ");
  state = State();
  EXPECT_TRUE(state->name_link_folders);
  EXPECT_EQ(state->name_link_folders_model, "claude-sonnet-5-5");
  EXPECT_TRUE(prefs()->GetBoolean(prefs::kNameLinkFolders));
  remote_->SetNameLinkFoldersModel("");
  EXPECT_EQ(State()->name_link_folders_model, "claude-haiku-5-5");

  // "Tidy Today": a switch and a model of its own.
  EXPECT_FALSE(state->tidy_today);
  EXPECT_EQ(state->tidy_today_model, "claude-haiku-5-5");
  remote_->SetTidyToday(true);
  remote_->SetTidyTodayModel("claude-opus-5-5");
  state = State();
  EXPECT_TRUE(state->tidy_today);
  EXPECT_EQ(state->tidy_today_model, "claude-opus-5-5");
  EXPECT_EQ(state->name_link_folders_model, "claude-haiku-5-5");
  EXPECT_TRUE(prefs()->GetBoolean(prefs::kTidyToday));
  remote_->SetTidyTodayModel(" ");
  EXPECT_EQ(State()->tidy_today_model, "claude-haiku-5-5");

  // "Shorter Tab Titles": the same, its own.
  EXPECT_FALSE(state->short_tab_titles);
  EXPECT_EQ(state->short_tab_titles_model, "claude-haiku-5-5");
  remote_->SetShortTabTitles(true);
  remote_->SetShortTabTitlesModel("claude-sonnet-5-5");
  state = State();
  EXPECT_TRUE(state->short_tab_titles);
  EXPECT_EQ(state->short_tab_titles_model, "claude-sonnet-5-5");
  EXPECT_EQ(state->tidy_today_model, "claude-haiku-5-5");
  EXPECT_TRUE(prefs()->GetBoolean(prefs::kShortTabTitles));
  EXPECT_TRUE(prefs()->GetBoolean(prefs::kTidyToday));
  remote_->SetShortTabTitles(false);
  remote_->SetShortTabTitlesModel("");
  state = State();
  EXPECT_FALSE(state->short_tab_titles);
  EXPECT_EQ(state->short_tab_titles_model, "claude-haiku-5-5");

  // "Answer Questions in the Command Bar": its own too, on Claude Sonnet by
  // default.
  EXPECT_FALSE(state->command_bar_ask);
  EXPECT_EQ(state->command_bar_ask_model, "claude-sonnet-5-5");
  remote_->SetCommandBarAsk(true);
  remote_->SetCommandBarAskModel("claude-opus-5-5");
  state = State();
  EXPECT_TRUE(state->command_bar_ask);
  EXPECT_EQ(state->command_bar_ask_model, "claude-opus-5-5");
  EXPECT_FALSE(state->short_tab_titles);
  EXPECT_TRUE(prefs()->GetBoolean(prefs::kCommandBarAsk));
  remote_->SetCommandBarAskModel("");
  EXPECT_EQ(State()->command_bar_ask_model, "claude-sonnet-5-5");
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

// Other AI providers (docs/design/ai-providers.md, "Settings"): each keeps
// its own key, base URL and models, under its own pref names; Anthropic's
// are the names they always had.
IN_PROC_BROWSER_TEST_F(ClaudeSettingsHandlerBrowserTest,
                       ProvidersKeepTheirOwnKeysAndModels) {
  mojom::SettingsStatePtr state = State();
  EXPECT_EQ(state->provider, "anthropic");
  ASSERT_EQ(state->providers.size(), 4u);
  EXPECT_EQ(state->providers[2]->id, "gemini");
  EXPECT_EQ(state->providers[2]->display_name, "Google Gemini");
  ASSERT_TRUE(SetKey(kGoodKey));
  const std::string anthropic_key = prefs()->GetString(prefs::kApiKey);
  ASSERT_FALSE(anthropic_key.empty());

  remote_->SetProvider("openai");
  state = State();
  EXPECT_EQ(state->provider, "openai");
  EXPECT_EQ(prefs()->GetString(prefs::kProvider), "openai");
  // Anthropic's key isn't OpenAI's.
  EXPECT_FALSE(state->has_key);
  EXPECT_FALSE(state->set_up);
  EXPECT_EQ(state->key_placeholder, "sk-…");
  EXPECT_EQ(state->default_base_url, "https://api.openai.com/v1");
  EXPECT_EQ(state->model, "gpt-6.1-terra");
  EXPECT_EQ(state->name_link_folders_model, "gpt-6-luna");
  EXPECT_EQ(state->command_bar_ask_model, "gpt-6-luna");

  ASSERT_TRUE(SetKey("sk-proj-openai-test-key-5678"));
  state = State();
  EXPECT_TRUE(state->has_key);
  EXPECT_TRUE(state->set_up);
  EXPECT_TRUE(base::EndsWith(state->key_hint, "5678"));
  EXPECT_FALSE(prefs()->GetString("compass.ai_providers.openai.api_key").empty());
  EXPECT_EQ(prefs()->GetString(prefs::kApiKey), anthropic_key);
  base::test::TestFuture<std::optional<std::string>> key;
  ClaudeCredentials::GetApiKey(browser()->GetProfile(), key.GetCallback());
  EXPECT_EQ(key.Get(), "sk-proj-openai-test-key-5678");

  // The setters write OpenAI's prefs, and leave Anthropic's alone.
  remote_->SetModel("gpt-6.2-sol");
  remote_->SetNameLinkFoldersModel("gpt-6.2-astra");
  remote_->SetTidyTodayModel(" gpt-6.2-sol ");
  remote_->SetShortTabTitlesModel("gpt-6.1-terra");
  remote_->SetCommandBarAskModel("gpt-6.2-astra");
  state = State();
  EXPECT_EQ(state->model, "gpt-6.2-sol");
  EXPECT_EQ(prefs()->GetString("compass.ai_providers.openai.model"), "gpt-6.2-sol");
  EXPECT_EQ(prefs()->GetString("compass.ai_providers.openai.name_link_folders_model"),
            "gpt-6.2-astra");
  EXPECT_EQ(prefs()->GetString("compass.ai_providers.openai.tidy_today_model"),
            "gpt-6.2-sol");
  EXPECT_EQ(prefs()->GetString("compass.ai_providers.openai.short_tab_titles_model"),
            "gpt-6.1-terra");
  EXPECT_EQ(prefs()->GetString("compass.ai_providers.openai.command_bar_ask_model"),
            "gpt-6.2-astra");
  EXPECT_EQ(prefs()->GetString(prefs::kModel), kDefaultModel);
  EXPECT_EQ(prefs()->GetString(prefs::kNameLinkFoldersModel),
            "claude-haiku-5-5");
  EXPECT_EQ(prefs()->GetString(prefs::kTidyTodayModel), "claude-haiku-5-5");
  EXPECT_EQ(prefs()->GetString(prefs::kShortTabTitlesModel),
            "claude-haiku-5-5");
  EXPECT_EQ(prefs()->GetString(prefs::kCommandBarAskModel),
            prefs::kDefaultAskModel);
  // Empty goes back to OpenAI's default.
  remote_->SetTidyTodayModel("");
  EXPECT_EQ(State()->tidy_today_model, "gpt-6-luna");

  // OpenAI's own API is kept as no URL; its auth style isn't Anthropic's.
  EXPECT_TRUE(SetEndpoint("https://api.openai.com/v1/",
                          mojom::AuthStyle::kBearer));
  EXPECT_EQ(State()->base_url, "");
  EXPECT_EQ(prefs()->GetInteger(prefs::kAuthStyle),
            static_cast<int>(AuthStyle::kApiKey));
  EXPECT_TRUE(SetEndpoint("https://proxy.example.com/openai/v1"));
  EXPECT_EQ(State()->base_url, "https://proxy.example.com/openai/v1");
  EXPECT_EQ(prefs()->GetString("compass.ai_providers.openai.base_url"),
            "https://proxy.example.com/openai/v1");
  EXPECT_EQ(prefs()->GetString(prefs::kBaseUrl), "");

  // An unknown provider changes nothing.
  remote_->SetProvider("mistral");
  EXPECT_EQ(State()->provider, "openai");

  // Back to Anthropic: its key and models as they were.
  remote_->SetProvider("anthropic");
  state = State();
  EXPECT_TRUE(state->has_key);
  EXPECT_EQ(state->key_hint, "sk-ant-…1234");
  EXPECT_EQ(state->model, kDefaultModel);
  EXPECT_EQ(state->tidy_today_model, "claude-haiku-5-5");
  EXPECT_EQ(state->base_url, "");
  base::test::TestFuture<std::optional<std::string>> anthropic;
  ClaudeCredentials::GetApiKey(browser()->GetProfile(),
                               anthropic.GetCallback());
  EXPECT_EQ(anthropic.Get(), kGoodKey);
}

// An OpenAI-compatible server: set up by its URL alone (a key is optional),
// its model typed, its features' models the chat's by default; Claude Code
// mode doesn't apply.
IN_PROC_BROWSER_TEST_F(ClaudeSettingsHandlerBrowserTest, CompatibleServer) {
  prefs()->SetInteger(prefs::kBackend, static_cast<int>(Backend::kClaudeCode));
  EXPECT_EQ(State()->backend, mojom::Backend::kClaudeCode);
  remote_->SetProvider("compatible");
  mojom::SettingsStatePtr state = State();
  EXPECT_EQ(state->backend, mojom::Backend::kApiKey);
  EXPECT_FALSE(state->set_up);
  EXPECT_EQ(state->default_base_url, "");
  EXPECT_EQ(state->model, "");
  EXPECT_EQ(state->name_link_folders_model, "");
  mojom::ConnectionResultPtr result = Test();
  EXPECT_FALSE(result->ok);
  EXPECT_EQ(result->error, "Enter the server's base URL first.");

  EXPECT_FALSE(SetEndpoint("http://gateway.example.com/v1"));
  const std::string url = embedded_test_server()->GetURL("/compat").spec();
  ASSERT_TRUE(SetEndpoint(url));
  state = State();
  EXPECT_TRUE(state->set_up);
  EXPECT_FALSE(state->has_key);
  EXPECT_EQ(prefs()->GetString("compass.ai_providers.compatible.base_url"),
            state->base_url);
  EXPECT_EQ(prefs()->GetString(prefs::kBaseUrl), "");
  result = Test();
  ASSERT_TRUE(result->ok) << result->error;
  ASSERT_EQ(result->models.size(), 1u);
  EXPECT_EQ(result->models[0]->id, "llama-4");

  remote_->SetModel("llama-4");
  // The setter has no reply: wait for it to arrive.
  remote_.FlushForTesting();
  EXPECT_EQ(prefs()->GetString("compass.ai_providers.compatible.model"),
            "llama-4");
  EXPECT_EQ(prefs()->GetString(prefs::kModel), kDefaultModel);

  // Empty is no server, not a bad URL.
  EXPECT_TRUE(SetEndpoint(""));
  EXPECT_FALSE(State()->set_up);
}

}  // namespace
}  // namespace compass::claude
