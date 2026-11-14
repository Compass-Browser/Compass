// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Ask in the command bar (docs/design/ai-sidebar.md, "Ask in the command
// bar"), with a fake Claude API.

#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/run_loop.h"
#include "base/strings/strcat.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/ui/views/claude/claude_panel.h"
#include "compass/browser/ui/views/command_bar/command_bar.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/http/http_status_code.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

constexpr char kKey[] = "sk-ant-api03-command-bar-ask-test-key";
constexpr char kAnswer[] = "Foxes are small canids.\nThey hunt mice.";

// The fake Claude API's stream for a reply of |text|, in two parts.
std::string Stream(const std::string& text) {
  auto event = [](const char* name, base::DictValue data) {
    return base::StrCat(
        {"event: ", name, "\ndata: ", *base::WriteJson(data), "\n\n"});
  };
  auto delta = [&](const std::string& part) {
    return event("content_block_delta",
                 base::DictValue()
                     .Set("type", "content_block_delta")
                     .Set("index", 0)
                     .Set("delta", base::DictValue()
                                       .Set("type", "text_delta")
                                       .Set("text", part)));
  };
  const size_t half = text.size() / 2;
  return base::StrCat(
      {event("message_start",
             base::DictValue().Set("type", "message_start")
                 .Set("message", base::DictValue()
                                     .Set("id", "msg_1")
                                     .Set("type", "message")
                                     .Set("role", "assistant")
                                     .Set("content", base::ListValue()))),
       event("content_block_start",
             base::DictValue()
                 .Set("type", "content_block_start")
                 .Set("index", 0)
                 .Set("content_block",
                      base::DictValue().Set("type", "text").Set("text", ""))),
       delta(text.substr(0, half)), delta(text.substr(half)),
       event("content_block_stop",
             base::DictValue().Set("type", "content_block_stop").Set("index",
                                                                     0)),
       event("message_delta",
             base::DictValue()
                 .Set("type", "message_delta")
                 .Set("delta",
                      base::DictValue().Set("stop_reason", "end_turn"))),
       event("message_stop", base::DictValue().Set("type", "message_stop"))});
}

constexpr char kRoot[] =
    "document.querySelector('compass-command-bar-app').shadowRoot";

// Types $1 into the field and waits for the results for it; returns the
// first row's title, and whether it has "About this page", as
// "title|about".
constexpr char kType[] = R"(
  (async (text) => {
    const root = document.querySelector('compass-command-bar-app').shadowRoot;
    const input = root.querySelector('#input');
    input.value = text;
    input.dispatchEvent(new InputEvent('input', {inputType: 'insertText'}));
    const first = () => root.querySelector('.result');
    for (let i = 0; i < 500; ++i) {
      const row = first();
      if (row && [...root.querySelectorAll('.result .subtitle')].some(
              s => s.textContent.includes(text.trim()))) {
        return row.querySelector('.title').textContent + '|' +
            !!row.querySelector('.about');
      }
      await new Promise(r => setTimeout(r, 20));
    }
    return '';
  })($1))";

// Presses $1 in the field.
constexpr char kPress[] = R"(
  ((key) => {
    document.querySelector('compass-command-bar-app').shadowRoot
        .querySelector('#input')
        .dispatchEvent(new KeyboardEvent('keydown', {key}));
  })($1))";

// The answer's text once it's complete and its Continue in Chat button is
// on, or '' after a few seconds.
constexpr char kAnswerText[] = R"(
  (async () => {
    const root = document.querySelector('compass-command-bar-app').shadowRoot;
    for (let i = 0; i < 500; ++i) {
      const text = root.querySelector('#answer .answer-text');
      const button = root.querySelector('#continueInChat');
      if (text && button && !button.disabled &&
          text.textContent.trim() === $1) {
        return text.textContent.trim();
      }
      await new Promise(r => setTimeout(r, 20));
    }
    return root.querySelector('#answer .answer-text')?.textContent || '';
  })())";

constexpr char kChatRoot[] =
    "document.querySelector('compass-claude-chat-app').shadowRoot";

class CommandBarAskBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        &CommandBarAskBrowserTest::Serve, base::Unretained(this)));
    ASSERT_TRUE(embedded_test_server()->Start());
    Profile* profile = browser()->GetProfile();
    profile->GetPrefs()->SetString(claude::prefs::kBaseUrl,
                                   embedded_test_server()->base_url().spec());
    base::test::TestFuture<bool> saved;
    claude::ClaudeCredentials::SetApiKey(profile, kKey, saved.GetCallback());
    ASSERT_TRUE(saved.Get());
  }

  std::unique_ptr<net::test_server::HttpResponse> Serve(
      const net::test_server::HttpRequest& request) {
    auto response = std::make_unique<net::test_server::BasicHttpResponse>();
    if (request.relative_url == "/v1/messages") {
      {
        base::AutoLock lock(lock_);
        bodies_.push_back(request.content);
      }
      response->set_code(net::HTTP_OK);
      response->set_content_type("text/event-stream");
      response->set_content(Stream(kAnswer));
      return response;
    }
    if (request.relative_url == "/fox/page.html") {
      response->set_content_type("text/html");
      response->set_content("<title>Fox page</title><p>Foxes.</p>");
      return response;
    }
    return nullptr;
  }

  std::vector<std::string> Bodies() {
    base::AutoLock lock(lock_);
    return bodies_;
  }

  CommandBar* bar() { return CommandBar::From(browser()); }

  content::WebContents* OpenBar() {
    chrome::ExecuteCommand(browser(), IDC_NEW_TAB);
    EXPECT_TRUE(base::test::RunUntil([&] { return bar()->IsShowing(); }));
    return bar()->web_contents_for_testing();
  }

  static std::string Text(content::WebContents* contents,
                          const char* root,
                          const std::string& query) {
    return content::EvalJs(contents, base::StrCat({"(", root,
                                                   ".querySelector('", query,
                                                   "')?.textContent || '')"
                                                   ".trim()"}))
        .ExtractString();
  }

  static void Wait(base::TimeDelta delay) {
    base::RunLoop loop;
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, loop.QuitClosure(), delay);
    loop.Run();
  }

  base::Lock lock_;
  std::vector<std::string> bodies_;
};

// A question gets "Ask Claude" first; Enter asks, with the tab's title and
// host; the answer streams in under the field; Continue in Chat opens the
// chat with the question and the answer as its start, and the next message
// goes on from them.
IN_PROC_BROWSER_TEST_F(CommandBarAskBrowserTest, AsksAndContinuesInChat) {
  browser()->GetProfile()->GetPrefs()->SetBoolean(
      claude::prefs::kCommandBarAsk, true);
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/fox/page.html")));
  content::WebContents* contents = OpenBar();
  ASSERT_TRUE(contents);

  EXPECT_EQ(content::EvalJs(contents,
                            content::JsReplace(kType, "what is a fox?")),
            "Ask Claude|true");
  ASSERT_TRUE(content::ExecJs(contents, content::JsReplace(kPress, "Enter")));
  EXPECT_EQ(content::EvalJs(contents,
                            content::JsReplace(kAnswerText, kAnswer)),
            kAnswer);
  EXPECT_TRUE(bar()->IsShowing());

  std::vector<std::string> bodies = Bodies();
  ASSERT_EQ(bodies.size(), 1u);
  std::optional<base::DictValue> body =
      base::JSONReader::ReadDict(bodies[0], base::JSON_PARSE_RFC);
  ASSERT_TRUE(body);
  // Its own model, Claude Sonnet by default; no tools.
  EXPECT_EQ(*body->FindString("model"), "claude-sonnet-5-5");
  EXPECT_FALSE(body->Find("tools"));
  const std::string user = *(*(*body->FindList("messages"))[0]
                                   .GetDict()
                                   .FindList("content"))[0]
                                .GetDict()
                                .FindString("text");
  EXPECT_NE(user.find("what is a fox?"), std::string::npos);
  // "About this page": the title and host, never the path.
  EXPECT_NE(user.find("Fox page"), std::string::npos);
  EXPECT_NE(user.find("127.0.0.1"), std::string::npos);
  EXPECT_EQ(user.find("/fox/"), std::string::npos);
  EXPECT_EQ(user.find("Foxes."), std::string::npos);

  // Enter now: Continue in Chat.
  ASSERT_TRUE(content::ExecJs(contents, content::JsReplace(kPress, "Enter")));
  EXPECT_TRUE(base::test::RunUntil([&] { return !bar()->IsShowing(); }));
  claude::ClaudePanel* panel = claude::ClaudePanel::From(browser());
  ASSERT_TRUE(panel);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return panel->IsShowing() && panel->web_contents_for_testing();
  }));
  content::WebContents* chat = panel->web_contents_for_testing();
  EXPECT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(
               chat, base::StrCat({"[...", kChatRoot,
                                   ".querySelectorAll('compass-claude-turn')]"
                                   ".length"}))
               .ExtractInt() == 2;
  }));
  EXPECT_EQ(content::EvalJs(
                chat, base::StrCat({"[...", kChatRoot,
                                    ".querySelectorAll('compass-claude-turn')]"
                                    "[0].shadowRoot.querySelector('.bubble')"
                                    ".textContent.trim()"})),
            "what is a fox?");
  // Nothing sent until the user writes.
  EXPECT_EQ(Bodies().size(), 1u);

  panel->Ask("And what do they eat?");
  EXPECT_TRUE(base::test::RunUntil([&] { return Bodies().size() == 2u; }));
  std::optional<base::DictValue> next =
      base::JSONReader::ReadDict(Bodies()[1], base::JSON_PARSE_RFC);
  ASSERT_TRUE(next);
  const base::ListValue& messages = *next->FindList("messages");
  ASSERT_EQ(messages.size(), 3u);
  EXPECT_NE(base::WriteJson(messages[0])->find("what is a fox?"),
            std::string::npos);
  EXPECT_EQ(*messages[1].GetDict().FindString("role"), "assistant");
  EXPECT_EQ(*(*messages[1].GetDict().FindList("content"))[0]
                 .GetDict()
                 .FindString("text"),
            kAnswer);
  EXPECT_NE(base::WriteJson(messages[2])->find("And what do they eat?"),
            std::string::npos);
}

// Off by default: no "Ask Claude" row at the top, and nothing sent; Enter
// does what it always did.
IN_PROC_BROWSER_TEST_F(CommandBarAskBrowserTest, NothingWithTheSwitchOff) {
  EXPECT_FALSE(browser()->GetProfile()->GetPrefs()->GetBoolean(
      claude::prefs::kCommandBarAsk));
  content::WebContents* contents = OpenBar();
  ASSERT_TRUE(contents);
  const std::string first =
      content::EvalJs(contents, content::JsReplace(kType, "what is a fox?"))
          .ExtractString();
  EXPECT_NE(first, "");
  EXPECT_NE(first, "Ask Claude|true");
  EXPECT_NE(first, "Ask Claude|false");
  EXPECT_EQ(Text(contents, kRoot, "#answer"), "");
  Wait(base::Milliseconds(300));
  EXPECT_TRUE(Bodies().empty());
}

}  // namespace
}  // namespace compass
