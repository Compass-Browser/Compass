// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/claude/claude_panel.h"

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/claude_assets.h"
#include "compass/browser/claude/claude_code.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/claude/claude_request.h"
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

// A reply that asks for a tool: the page's text.
constexpr char kToolReply[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_2\","
    "\"type\":\"message\",\"role\":\"assistant\",\"content\":[]}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,"
    "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_1\","
    "\"name\":\"get_page_text\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
    "\"delta\":{\"type\":\"input_json_delta\","
    "\"partial_json\":\"{\\\"max_chars\\\": 2000}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
    "\"tool_use\"}}\n\n"
    "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";

// A reply that saves a file (save_file).
constexpr char kSaveReply[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_3\","
    "\"type\":\"message\",\"role\":\"assistant\",\"content\":[]}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,"
    "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_save\","
    "\"name\":\"save_file\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
    "\"delta\":{\"type\":\"input_json_delta\","
    "\"partial_json\":\"{\\\"name\\\": \\\"fox.md\\\", \\\"content\\\": "
    "\\\"The quick fox\\\"}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
    "\"tool_use\"}}\n\n"
    "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";

// How often |needle| is in |text|.
size_t Count(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

#if BUILDFLAG(IS_POSIX)
// A fake `claude` for Claude Code mode: it saves its arguments and the
// messages it gets, calls get_page_text through the MCP server its
// --mcp-config names (the real compass-mcp --chat), and answers each
// message in stream-json as Claude Code does: a tool step, its result, then
// the page's first words.
constexpr char kFakeClaudeCode[] = R"PY(#!/usr/bin/env python3
import json, os, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
with open(os.path.join(here, 'args.json'), 'w') as f:
    json.dump(args, f)
config = json.loads(args[args.index('--mcp-config') + 1])
server = config['mcpServers']['compass']
mcp = subprocess.Popen([server['command']] + server['args'],
                       stdin=subprocess.PIPE, stdout=subprocess.PIPE)

def rpc(id, method, params):
    mcp.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': id,
                                 'method': method,
                                 'params': params}) + '\n').encode())
    mcp.stdin.flush()
    return json.loads(mcp.stdout.readline())

rpc(1, 'initialize', {'protocolVersion': '2025-06-18',
                      'clientInfo': {'name': 'claude-code'}})

def out(obj):
    sys.stdout.write(json.dumps(obj) + '\n')
    sys.stdout.flush()

def event(e):
    out({'type': 'stream_event', 'event': e, 'parent_tool_use_id': None})

out({'type': 'system', 'subtype': 'init', 'session_id': 'fake-session'})
step = 0
for line in sys.stdin:
    with open(os.path.join(here, 'messages.jsonl'), 'a') as f:
        f.write(line)
    step += 1
    tool_id = 'toolu_%d' % step
    event({'type': 'message_start', 'message': {
        'id': 'msg_a', 'role': 'assistant', 'content': []}})
    event({'type': 'content_block_start', 'index': 0, 'content_block': {
        'type': 'tool_use', 'id': tool_id,
        'name': 'mcp__compass__get_page_text', 'input': {}}})
    event({'type': 'content_block_stop', 'index': 0})
    event({'type': 'message_delta', 'delta': {'stop_reason': 'tool_use'}})
    event({'type': 'message_stop'})
    result = rpc(10 + step, 'tools/call',
                 {'name': 'get_page_text', 'arguments': {}})['result']
    out({'type': 'user', 'message': {'role': 'user', 'content': [{
        'type': 'tool_result', 'tool_use_id': tool_id,
        'content': result['content'],
        'is_error': result.get('isError', False)}]},
        'parent_tool_use_id': None})
    text = result['content'][0]['text']
    words = text[text.index('The quick'):][:19]
    event({'type': 'message_start', 'message': {
        'id': 'msg_b', 'role': 'assistant', 'content': []}})
    event({'type': 'content_block_start', 'index': 0,
           'content_block': {'type': 'text', 'text': ''}})
    event({'type': 'content_block_delta', 'index': 0, 'delta': {
        'type': 'text_delta', 'text': 'It says: ' + words}})
    event({'type': 'content_block_stop', 'index': 0})
    event({'type': 'message_delta', 'delta': {'stop_reason': 'end_turn'}})
    event({'type': 'message_stop'})
    out({'type': 'result', 'subtype': 'success', 'is_error': False})
)PY";
#endif  // BUILDFLAG(IS_POSIX)

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
    bool wants_tool = false;
    {
      base::AutoLock lock(lock_);
      last_body_ = request.content;
      bodies_.push_back(request.content);
      // "Use a tool" asks for one, until its result comes back.
      wants_tool =
          request.content.find("Use a tool") != std::string::npos &&
          request.content.find("tool_result") == std::string::npos;
      const auto key = request.headers.find("x-api-key");
      last_key_ = key != request.headers.end() ? key->second : "";
    }
    response->set_code(net::HTTP_OK);
    response->set_content_type("text/event-stream");
    // "Save a file" saves one, once per such message: until its result.
    const bool wants_save =
        Count(request.content, "Save a file") >
        Count(request.content, "\"tool_use_id\":\"toolu_save\"");
    response->set_content(wants_save   ? kSaveReply
                          : wants_tool ? kToolReply
                                       : kReply);
    return response;
  }

  std::string LastBody() {
    base::AutoLock lock(lock_);
    return last_body_;
  }

  std::vector<std::string> Bodies() {
    base::AutoLock lock(lock_);
    return bodies_;
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
  std::vector<std::string> bodies_;
  std::string last_key_;
};

IN_PROC_BROWSER_TEST_F(ClaudePanelBrowserTest, WithoutAKeyAsksForOne) {
  ASSERT_TRUE(panel());
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser());
  const gfx::Rect page_before =
      browser_view->contents_web_view_for_testing()->GetBoundsInScreen();
  content::WebContents* contents = Open();
  ASSERT_TRUE(contents);
  EXPECT_TRUE(base::test::RunUntil([&] {
    return Text(contents, "#openSettings") == "Add an API key";
  }));
  // No toolbar-colored box behind the page and the panel, and no shadow box
  // around the page: upstream's main background region and shadow overlay
  // stay hidden in Compass's toolbarless window, and the page keeps its top
  // and height (only its width gives way to the panel).
  for (views::View* child : browser_view->children()) {
    const std::string_view name = child->GetClassName();
    if (name == "MainBackgroundRegionView" || name == "ShadowOverlayView") {
      EXPECT_FALSE(child->GetVisible()) << name;
    }
  }
  const gfx::Rect page_after =
      browser_view->contents_web_view_for_testing()->GetBoundsInScreen();
  EXPECT_EQ(page_after.y(), page_before.y());
  EXPECT_EQ(page_after.height(), page_before.height());
  EXPECT_LT(page_after.width(), page_before.width());
  // The chat is as tall as the page (GetContainerBounds() counts from
  // another origin, so heights).
  EXPECT_EQ(contents->GetContainerBounds().height(), page_after.height());
  // Again: it closes.
  EXPECT_TRUE(chrome::ExecuteCommand(browser(), kToggleClaudeCommand));
  EXPECT_TRUE(base::test::RunUntil([&] { return !panel()->IsShowing(); }));
}

// The header's model menu sets the model, and shows one picked elsewhere
// (settings, a gateway's id) too.
IN_PROC_BROWSER_TEST_F(ClaudePanelBrowserTest, ModelMenuSetsTheModel) {
  SetUpClaude();
  PrefService* prefs = browser()->GetProfile()->GetPrefs();
  content::WebContents* contents = Open();
  ASSERT_TRUE(contents);
  const std::string menu = std::string(kRoot) + ".querySelector('#model')";
  EXPECT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(contents, menu + "?.value").ExtractString() ==
           "claude-opus-5-5";
  }));
  ASSERT_TRUE(content::ExecJs(
      contents, "{ const m = " + menu +
                    "; m.value = 'claude-sonnet-5-5';"
                    " m.dispatchEvent(new Event('change')); }"));
  EXPECT_TRUE(base::test::RunUntil([&] {
    return prefs->GetString(prefs::kModel) == "claude-sonnet-5-5";
  }));

  prefs->SetString(prefs::kModel, "my-gateway-model");
  EXPECT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(contents, menu + "?.value").ExtractString() ==
           "my-gateway-model";
  }));
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
  // The message: the browser's context, the page, the question.
  const base::ListValue& content = *messages[0].GetDict().FindList("content");
  ASSERT_EQ(content.size(), 3u);
  EXPECT_TRUE(base::StartsWith(*content[0].GetDict().FindString("text"),
                               "<context>"));
  const std::string& page = *content[1].GetDict().FindString("text");
  EXPECT_TRUE(base::StartsWith(page, "<page url=\""));
  EXPECT_NE(page.find("The quick brown fox"), std::string::npos);
  EXPECT_EQ(*content[2].GetDict().FindString("text"),
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
  // Context and question, no page; the first message is sent as it was, and
  // so is the system prompt (thinking is bound to both).
  EXPECT_EQ(history[2].GetDict().FindList("content")->size(), 2u);
  EXPECT_EQ(history[0], messages[0]);
  EXPECT_EQ(*second->FindList("system"), *body->FindList("system"));
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

// Closing the panel keeps the conversation: reopened, it's the same page
// with the same turns, and the next message goes on from them.
IN_PROC_BROWSER_TEST_F(ClaudePanelBrowserTest, KeepsTheChatWhenClosed) {
  SetUpClaude();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/page.html")));
  panel()->Ask("Which animal jumps?");
  // The page is there once its request is.
  EXPECT_TRUE(base::test::RunUntil([&] {
    return panel()->IsShowing() && panel()->web_contents_for_testing() &&
           LastBody().find("Which animal jumps?") != std::string::npos;
  }));
  content::WebContents* contents = panel()->web_contents_for_testing();
  EXPECT_TRUE(base::test::RunUntil([&] {
    return TurnText(contents, "assistant", ".markdown") ==
           "Hello from the fake API.";
  }));

  EXPECT_TRUE(chrome::ExecuteCommand(browser(), kToggleClaudeCommand));
  EXPECT_TRUE(base::test::RunUntil([&] { return !panel()->IsShowing(); }));

  EXPECT_EQ(Open(), contents);
  EXPECT_EQ(TurnText(contents, "assistant", ".markdown"),
            "Hello from the fake API.");
  EXPECT_EQ(TurnText(contents, "user", ".attached"), "Fox page");

  // Closed again, a question goes to the same conversation.
  EXPECT_TRUE(chrome::ExecuteCommand(browser(), kToggleClaudeCommand));
  EXPECT_TRUE(base::test::RunUntil([&] { return !panel()->IsShowing(); }));
  panel()->Ask("And which one is lazy?");
  EXPECT_TRUE(base::test::RunUntil([&] {
    return panel()->IsShowing() &&
           LastBody().find("And which one is lazy?") != std::string::npos;
  }));
  EXPECT_EQ(panel()->web_contents_for_testing(), contents);
  std::optional<base::DictValue> body =
      base::JSONReader::ReadDict(LastBody(), base::JSON_PARSE_RFC);
  ASSERT_TRUE(body);
  EXPECT_EQ(body->FindList("messages")->size(), 3u);
}

// The agent loop: a tool_use reply runs the tool in the browser and sends
// its result back, then the final reply shows. docs/design/claude.md, D4.
IN_PROC_BROWSER_TEST_F(ClaudePanelBrowserTest, RunsToolsAndSendsResults) {
  SetUpClaude();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/page.html")));
  content::WebContents* contents = Open();
  ASSERT_TRUE(contents);
  ASSERT_TRUE(content::ExecJs(contents, std::string("const root = ") + kRoot +
                                            R"(;
      root.querySelector('#attach').click();  // Not attached: use the tool.
      const input = root.querySelector('#input');
      input.value = 'Use a tool to read the page.';
      input.dispatchEvent(new InputEvent('input'));
      input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));)"));

  EXPECT_TRUE(base::test::RunUntil([&] {
    return TurnText(contents, "assistant", ".markdown") ==
           "Hello from the fake API.";
  }));
  const std::vector<std::string> bodies = Bodies();
  ASSERT_EQ(bodies.size(), 2u);
  // The first request offers the tools.
  std::optional<base::DictValue> first =
      base::JSONReader::ReadDict(bodies[0], base::JSON_PARSE_RFC);
  ASSERT_TRUE(first);
  EXPECT_FALSE(first->FindList("tools")->empty());
  // The second: Claude's tool_use, then the browser's result, with the
  // page's text.
  std::optional<base::DictValue> second =
      base::JSONReader::ReadDict(bodies[1], base::JSON_PARSE_RFC);
  ASSERT_TRUE(second);
  const base::ListValue& messages = *second->FindList("messages");
  ASSERT_EQ(messages.size(), 3u);
  const base::DictValue& use =
      (*messages[1].GetDict().FindList("content"))[0].GetDict();
  EXPECT_EQ(*use.FindString("type"), "tool_use");
  EXPECT_EQ(*use.FindIntByDottedPath("input.max_chars"), 2000);
  const base::DictValue& result =
      (*messages[2].GetDict().FindList("content"))[0].GetDict();
  EXPECT_EQ(*result.FindString("type"), "tool_result");
  EXPECT_EQ(*result.FindString("tool_use_id"), "toolu_1");
  EXPECT_EQ(result.FindBool("is_error"), false);
  const std::string& text =
      *(*result.FindList("content"))[0].GetDict().FindString("text");
  EXPECT_NE(text.find("The quick brown fox"), std::string::npos);

  // The tool's row shows in the reply.
  EXPECT_EQ(content::EvalJs(contents, std::string("[...") + kRoot +
                                          ".querySelectorAll("
                                          "'compass-claude-turn')].map(t => "
                                          "t.shadowRoot.querySelector("
                                          "'.tool-label')?.textContent)"
                                          ".filter(Boolean).join()"),
            "Read the page's text");
}

// save_file asks in the panel first: Allow for this chat stops the
// questions until New chat; Deny writes nothing (docs/design/claude.md,
// "Files").
IN_PROC_BROWSER_TEST_F(ClaudePanelBrowserTest, AsksBeforeSaving) {
  SetUpClaude();
  base::ScopedTempDir dir;
  base::FilePath folder;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(dir.CreateUniqueTempDir());
    folder = dir.GetPath().AppendASCII("Saved");
    ASSERT_TRUE(base::CreateDirectory(folder));
  }
  PrefService* prefs = browser()->GetProfile()->GetPrefs();
  prefs->SetInteger(prefs::kFilesAccess,
                    static_cast<int>(prefs::FilesAccess::kSave));
  prefs->SetString(prefs::kFilesFolder, folder.AsUTF8Unsafe());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/page.html")));
  content::WebContents* contents = Open();
  ASSERT_TRUE(contents);
  const std::string root = std::string("const root = ") + kRoot + ";";
  auto send = [&](const std::string& text) {
    ASSERT_TRUE(content::ExecJs(contents, root + R"(
        const input = root.querySelector('#input');
        input.value = ')" + text + R"(';
        input.dispatchEvent(new InputEvent('input'));
        input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));)"));
  };
  auto approval_title = [&] {
    return content::EvalJs(contents,
                            std::string("(") + kRoot +
                                ".querySelector('#approvalTitle')"
                                "?.textContent.trim() || '')")
        .ExtractString();
  };
  auto click = [&](const std::string& id) {
    ASSERT_TRUE(content::ExecJs(
        contents, root + "root.querySelector('#" + id + "').click();"));
  };
  auto exists = [&](const std::string& name) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    return base::PathExists(folder.AppendASCII(name));
  };
  // The reply ended: Send is back instead of Stop.
  auto idle = [&] {
    return content::EvalJs(contents, std::string("!!") + kRoot +
                                         ".querySelector('#send')")
        .ExtractBool();
  };

  send("Save a file.");
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return approval_title() == "Save \u201cfox.md\u201d in Saved"; }));
  EXPECT_FALSE(exists("fox.md"));
  click("allowForChat");
  ASSERT_TRUE(base::test::RunUntil([&] {
    return TurnText(contents, "assistant", ".markdown") ==
           "Hello from the fake API.";
  }));
  EXPECT_TRUE(exists("fox.md"));
  EXPECT_EQ(approval_title(), "");
  ASSERT_TRUE(base::test::RunUntil(idle));

  // Allowed for this chat: no question.
  send("Save a file again.");
  ASSERT_TRUE(base::test::RunUntil([&] { return exists("fox (1).md"); }));
  EXPECT_EQ(approval_title(), "");
  ASSERT_TRUE(base::test::RunUntil([&] { return Bodies().size() == 4u; }));
  ASSERT_TRUE(base::test::RunUntil(idle));

  // A new chat asks again; Deny.
  click("newChat");
  send("Save a file.");
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return approval_title() == "Save \u201cfox.md\u201d in Saved"; }));
  click("deny");
  ASSERT_TRUE(base::test::RunUntil([&] {
    return LastBody().find("The user didn't allow it.") != std::string::npos;
  }));
  EXPECT_FALSE(exists("fox (2).md"));
}

#if BUILDFLAG(IS_POSIX)
// Claude Code mode: the message goes to `claude` (a fake one), which runs
// Compass's tool through compass-mcp --chat on the chat's tab, with no
// question asked; the panel shows the tool's row and the reply.
IN_PROC_BROWSER_TEST_F(ClaudePanelBrowserTest, RunsOnClaudeCode) {
  base::ScopedTempDir dir;
  base::FilePath fake;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(dir.CreateUniqueTempDir());
    fake = dir.GetPath().Append("claude");
    ASSERT_TRUE(base::WriteFile(fake, kFakeClaudeCode));
    ASSERT_TRUE(base::SetPosixFilePermissions(fake, 0755));
  }
  PrefService* prefs = browser()->GetProfile()->GetPrefs();
  prefs->SetInteger(prefs::kBackend, static_cast<int>(Backend::kClaudeCode));
  prefs->SetString(prefs::kClaudeCodePath, fake.value());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/page.html")));
  content::WebContents* contents = Open();
  ASSERT_TRUE(contents);
  ASSERT_TRUE(content::ExecJs(contents, std::string("const root = ") + kRoot +
                                            R"(;
      root.querySelector('#attach').click();  // Not attached: use the tool.
      const input = root.querySelector('#input');
      input.value = 'What does the page say?';
      input.dispatchEvent(new InputEvent('input'));
      input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));)"));

  EXPECT_TRUE(base::test::RunUntil([&] {
    return TurnText(contents, "assistant", ".markdown") ==
           "It says: The quick brown fox";
  }));
  // The tool's row, with Compass's name for it.
  EXPECT_EQ(content::EvalJs(contents, std::string("[...") + kRoot +
                                          ".querySelectorAll("
                                          "'compass-claude-turn')].map(t => "
                                          "t.shadowRoot.querySelector("
                                          "'.tool-label')?.textContent)"
                                          ".filter(Boolean).join()"),
            "Read the page's text");
  // No request went to the API.
  EXPECT_TRUE(Bodies().empty());

  base::ScopedAllowBlockingForTesting allow_blocking;
  std::string args_json;
  ASSERT_TRUE(
      base::ReadFileToString(dir.GetPath().Append("args.json"), &args_json));
  std::optional<base::Value> args =
      base::JSONReader::Read(args_json, base::JSON_PARSE_RFC);
  ASSERT_TRUE(args && args->is_list());
  auto has = [&](const std::string& flag, const std::string& value) {
    const base::ListValue& list = args->GetList();
    for (size_t i = 0; i + 1 < list.size(); ++i) {
      if (list[i].GetString() == flag) {
        return list[i + 1].GetString() == value;
      }
    }
    return false;
  };
  EXPECT_TRUE(has("--tools", ""));
  EXPECT_TRUE(has("--model", prefs->GetString(prefs::kModel)));
  EXPECT_TRUE(has("--allowedTools", "mcp__compass"));
  EXPECT_TRUE(has("--system-prompt", SystemPromptText()));

  // Claude Code got the user's message alone, with Compass's context.
  std::string messages;
  ASSERT_TRUE(base::ReadFileToString(dir.GetPath().Append("messages.jsonl"),
                                     &messages));
  std::optional<base::DictValue> message = base::JSONReader::ReadDict(
      messages.substr(0, messages.find('\n')), base::JSON_PARSE_RFC);
  ASSERT_TRUE(message);
  EXPECT_EQ(*message->FindString("type"), "user");
  const base::ListValue& content =
      *message->FindListByDottedPath("message.content");
  EXPECT_NE(content.front().GetDict().FindString("text")->find("<context>"),
            std::string::npos);
  EXPECT_EQ(*content.back().GetDict().FindString("text"),
            "What does the page say?");
}
#endif  // BUILDFLAG(IS_POSIX)

}  // namespace
}  // namespace compass::claude
