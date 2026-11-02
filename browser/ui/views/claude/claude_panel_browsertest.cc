// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/claude/claude_panel.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/claude_assets.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/http/http_status_code.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude {
namespace {

constexpr char kKey[] = "sk-ant-api03-panel-test-key-9876";

constexpr char kReply[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\","
    "\"type\":\"message\",\"role\":\"assistant\",\"content\":[]}}\n\n"
    "event: ping\ndata: {\"type\":\"ping\"}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,"
    "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
    "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello from **the\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
    "\"delta\":{\"type\":\"text_delta\",\"text\":\" fake** API.\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
    "\"end_turn\"},\"usage\":{\"output_tokens\":7}}\n\n"
    "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";

constexpr char kRoot[] =
    "document.querySelector('compass-claude-chat-app').shadowRoot";

class ClaudePanelBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        &ClaudePanelBrowserTest::Serve, base::Unretained(this)));
    ASSERT_TRUE(embedded_test_server()->Start());
    if (SidePanelUI* ui = SidePanelUI::From(browser())) {
      ui->DisableAnimationsForTesting();
      ui->SetNoDelaysForTesting(true);
    }
  }

  std::unique_ptr<net::test_server::HttpResponse> Serve(
      const net::test_server::HttpRequest& request) {
    auto response = std::make_unique<net::test_server::BasicHttpResponse>();
    if (request.relative_url == "/page.html") {
      response->set_content_type("text/html");
      response->set_content(
          "<title>Fox page</title><main><p>The quick brown fox jumps over "
          "the lazy dog.</p></main>");
      return response;
    }
    if (request.relative_url != "/v1/messages") {
      return nullptr;
    }
    {
      base::AutoLock lock(lock_);
      last_body_ = request.content;
      const auto key = request.headers.find("x-api-key");
      last_key_ = key != request.headers.end() ? key->second : "";
    }
    response->set_code(net::HTTP_OK);
    response->set_content_type("text/event-stream");
    response->set_content(kReply);
    return response;
  }

  std::string LastBody() {
    base::AutoLock lock(lock_);
    return last_body_;
  }

  std::string LastKey() {
    base::AutoLock lock(lock_);
    return last_key_;
  }

  void SetUpClaude() {
    PrefService* prefs = browser()->GetProfile()->GetPrefs();
    prefs->SetString(prefs::kBaseUrl, embedded_test_server()->base_url().spec());
    base::test::TestFuture<bool> saved;
    ClaudeCredentials::SetApiKey(browser()->GetProfile(), kKey,
                                 saved.GetCallback());
    ASSERT_TRUE(saved.Get());
  }

  ClaudePanel* panel() { return ClaudePanel::From(browser()); }

  // Cmd+Shift+E, until the page shows.
  content::WebContents* Open() {
    EXPECT_TRUE(chrome::ExecuteCommand(browser(), kToggleClaudeCommand));
    EXPECT_TRUE(base::test::RunUntil([&] {
      return panel()->IsShowing() && panel()->web_contents_for_testing();
    }));
    content::WebContents* contents = panel()->web_contents_for_testing();
    EXPECT_TRUE(content::WaitForLoadStop(contents));
    return contents;
  }

  // The text of |query| in the last turn of |role| ("user", "assistant").
  std::string TurnText(content::WebContents* contents,
                       const std::string& role,
                       const std::string& query) {
    return content::EvalJs(
               contents,
               content::JsReplace(
                   std::string("(() => { const turns = [...") + kRoot +
                       ".querySelectorAll('compass-claude-turn')]"
                       ".map(t => t.shadowRoot)"
                       ".filter(r => r.querySelector('.turn.' + $1));"
                       "const last = turns[turns.length - 1];"
                       "return (last?.querySelector($2)?.textContent || '')"
                       ".trim(); })()",
                   role, query))
        .ExtractString();
  }

  std::string Text(content::WebContents* contents, const std::string& query) {
    return content::EvalJs(contents, std::string("(") + kRoot +
                                         ".querySelector('" + query +
                                         "')?.textContent || '').trim()")
        .ExtractString();
  }

  base::Lock lock_;
  std::string last_body_;
  std::string last_key_;
};

IN_PROC_BROWSER_TEST_F(ClaudePanelBrowserTest, WithoutAKeyAsksForOne) {
  ASSERT_TRUE(panel());
  content::WebContents* contents = Open();
  ASSERT_TRUE(contents);
  EXPECT_TRUE(base::test::RunUntil([&] {
    return Text(contents, "#openSettings") == "Add an API key";
  }));
  // Again: it closes.
  EXPECT_TRUE(chrome::ExecuteCommand(browser(), kToggleClaudeCommand));
  EXPECT_TRUE(base::test::RunUntil([&] { return !panel()->IsShowing(); }));
}

IN_PROC_BROWSER_TEST_F(ClaudePanelBrowserTest, AsksAboutThePage) {
  SetUpClaude();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/page.html")));
  content::WebContents* contents = Open();
  ASSERT_TRUE(contents);
  // The page is offered.
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return Text(contents, "#attach") == "Fox page"; }));

  ASSERT_TRUE(content::ExecJs(contents, std::string("const root = ") + kRoot +
                                            R"(;
      const input = root.querySelector('#input');
      input.value = 'What animal is this about?';
      input.dispatchEvent(new InputEvent('input'));
      input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));)"));

  // The reply streams in, rendered.
  EXPECT_TRUE(base::test::RunUntil([&] {
    return TurnText(contents, "assistant", ".markdown") ==
           "Hello from the fake API.";
  }));
  EXPECT_EQ(TurnText(contents, "assistant", ".markdown strong"),
            "the fake");
  EXPECT_EQ(TurnText(contents, "user", ".attached"), "Fox page");

  // The request: the key in its header, the browser's model and prompt, the
  // page's text with the question.
  EXPECT_EQ(LastKey(), kKey);
  std::optional<base::DictValue> body =
      base::JSONReader::ReadDict(LastBody(), base::JSON_PARSE_RFC);
  ASSERT_TRUE(body);
  EXPECT_EQ(*body->FindString("model"), kDefaultModel);
  EXPECT_EQ(body->FindBool("stream"), true);
  EXPECT_EQ(*(*body->FindList("system"))[0].GetDict().FindString("text"),
            assets::kSystemPrompt);
  const base::ListValue& messages = *body->FindList("messages");
  ASSERT_EQ(messages.size(), 1u);
  const base::ListValue& content = *messages[0].GetDict().FindList("content");
  ASSERT_EQ(content.size(), 2u);
  const std::string& page = *content[0].GetDict().FindString("text");
  EXPECT_TRUE(base::StartsWith(page, "<page url=\""));
  EXPECT_NE(page.find("The quick brown fox"), std::string::npos);
  EXPECT_EQ(*content[1].GetDict().FindString("text"),
            "What animal is this about?");
  // A gateway (the test server) gets no fallback.
  EXPECT_FALSE(body->contains("fallbacks"));

  // A second question sends the conversation, without the page again.
  ASSERT_TRUE(content::ExecJs(contents, std::string("const root = ") + kRoot +
                                            R"(;
      const input = root.querySelector('#input');
      input.value = 'And the dog?';
      input.dispatchEvent(new InputEvent('input'));
      input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));)"));
  EXPECT_TRUE(base::test::RunUntil([&] {
    std::optional<base::DictValue> second =
        base::JSONReader::ReadDict(LastBody(), base::JSON_PARSE_RFC);
    return second && second->FindList("messages")->size() == 3u;
  }));
  std::optional<base::DictValue> second =
      base::JSONReader::ReadDict(LastBody(), base::JSON_PARSE_RFC);
  const base::ListValue& history = *second->FindList("messages");
  EXPECT_EQ(*history[1].GetDict().FindString("role"), "assistant");
  EXPECT_EQ(history[2].GetDict().FindList("content")->size(), 1u);
}

// The command bar's "Ask Claude".
IN_PROC_BROWSER_TEST_F(ClaudePanelBrowserTest, AskOpensAndSends) {
  SetUpClaude();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/page.html")));

  // Closed: the question waits for the page, then goes with the page.
  panel()->Ask("Which animal jumps?");
  EXPECT_TRUE(base::test::RunUntil([&] {
    return panel()->IsShowing() && panel()->web_contents_for_testing() &&
           LastBody().find("Which animal jumps?") != std::string::npos;
  }));
  content::WebContents* contents = panel()->web_contents_for_testing();
  EXPECT_TRUE(base::test::RunUntil([&] {
    return TurnText(contents, "assistant", ".markdown") ==
           "Hello from the fake API.";
  }));
  EXPECT_EQ(TurnText(contents, "user", ".attached"), "Fox page");

  // Open: straight to the page, after the first exchange.
  panel()->Ask("And which one is lazy?");
  EXPECT_TRUE(base::test::RunUntil([&] {
    return LastBody().find("And which one is lazy?") != std::string::npos;
  }));
  std::optional<base::DictValue> body =
      base::JSONReader::ReadDict(LastBody(), base::JSON_PARSE_RFC);
  ASSERT_TRUE(body);
  EXPECT_EQ(body->FindList("messages")->size(), 3u);
}

}  // namespace
}  // namespace compass::claude
