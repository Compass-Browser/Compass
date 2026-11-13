// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_tools.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/threading/thread_restrictions.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "build/build_config.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/claude/claude_activity.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/claude/claude_tabs.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/window_open_disposition.h"

namespace compass::claude {
namespace {

using ::testing::HasSubstr;

constexpr char kStyledPage[] = R"(<!doctype html>
<title>Styled</title>
<style>
  .card { padding: 10px; }
  #box { display: flex; width: 180px; height: 30px; color: red; }
</style>
<div id="box" class="card">Box</div>
<button id="go" style="margin-top: 4px">Go</button>
<script>
  document.getElementById('box').addEventListener(
      'click', function boxClicked() {}, {passive: true});
</script>)";

constexpr char kPage[] = R"(<!doctype html>
<title>Tools page</title>
<h1>Form</h1>
<button id="press" onclick="window.clicks = (window.clicks || 0) + 1;
    window.trusted = event.isTrusted;">Press me</button>
<input id="name" aria-label="Your name">
<select id="color" aria-label="Color">
  <option value="r">Red</option><option value="g">Green</option>
</select>
<label><input type="checkbox" id="agree"> I agree</label>
<script>
  console.log('loaded', 42);
  setTimeout(() => { throw new Error('boom'); }, 0);
</script>)";

class ClaudeToolsBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        [](const net::test_server::HttpRequest& request)
            -> std::unique_ptr<net::test_server::HttpResponse> {
          if (request.relative_url.starts_with("/api.json")) {
            auto response =
                std::make_unique<net::test_server::BasicHttpResponse>();
            response->set_content_type("application/json");
            response->set_content(R"({"name":"Ada","token":"t0ps3cret"})");
            return response;
          }
          if (request.relative_url == "/styled.html") {
            auto response =
                std::make_unique<net::test_server::BasicHttpResponse>();
            response->set_content_type("text/html");
            response->set_content(kStyledPage);
            return response;
          }
          if (request.relative_url != "/tools.html" &&
              request.relative_url != "/other.html") {
            return nullptr;
          }
          auto response =
              std::make_unique<net::test_server::BasicHttpResponse>();
          response->set_content_type("text/html");
          response->set_content(request.relative_url == "/tools.html"
                                    ? kPage
                                    : "<title>Other</title><p>Other page");
          return response;
        }));
    ASSERT_TRUE(embedded_test_server()->Start());
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL("/tools.html")));
    tools_ = std::make_unique<ClaudeTools>(
        std::make_unique<ClaudeTabs>(browser()->GetProfile(), browser()));
    tools_->SetTarget(contents());
  }

  void TearDownOnMainThread() override {
    tools_.reset();
    InProcessBrowserTest::TearDownOnMainThread();
  }

  content::WebContents* contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  ToolResult Run(const std::string& name, const std::string& input = "{}") {
    std::optional<base::DictValue> dict =
        base::JSONReader::ReadDict(input, base::JSON_PARSE_RFC);
    EXPECT_TRUE(dict) << input;
    base::test::TestFuture<ToolResult> result;
    tools_->Run(name, std::move(*dict), result.GetCallback());
    return result.Take();
  }

  // The result's first text block.
  static std::string Text(const ToolResult& result) {
    for (const base::Value& block : result.content) {
      if (const std::string* text = block.GetDict().FindString("text")) {
        return *text;
      }
    }
    return std::string();
  }

  // The ref read_page gives the line containing |name|.
  std::string RefOf(const std::string& name) {
    const std::string tree = Text(Run("read_page"));
    for (const std::string& line : base::SplitString(
             tree, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      if (line.find(name) == std::string::npos) {
        continue;
      }
      const size_t at = line.find("[ref=");
      if (at != std::string::npos) {
        return line.substr(at + 5, line.find(']', at) - at - 5);
      }
      // "e12 button ..." style.
      for (const std::string& word :
           base::SplitString(line, " ", base::TRIM_WHITESPACE,
                             base::SPLIT_WANT_NONEMPTY)) {
        if (word.size() > 1 && word[0] == 'e' &&
            base::ContainsOnlyChars(word.substr(1), "0123456789")) {
          return word;
        }
      }
    }
    ADD_FAILURE() << "No ref for " << name << " in:\n" << tree;
    return std::string();
  }

  void SetBrowserScope(bool on) {
    browser()->GetProfile()->GetPrefs()->SetBoolean(prefs::kBrowserScope, on);
  }

  std::string IdOf(content::WebContents* contents) {
    return ClaudeTabs(browser()->GetProfile(), browser()).IdOf(contents);
  }

  // The tab id in |result|'s "Opened tab tN: …".
  static std::string OpenedId(const ToolResult& result) {
    const std::string text = Text(result);
    const size_t at = text.find("Opened tab ");
    if (at == std::string::npos) {
      return std::string();
    }
    const size_t start = at + strlen("Opened tab ");
    return text.substr(start, text.find(':', start) - start);
  }

  // The developer tools, on.
  void AllowDevTools() {
    tools_->SetDevTools(base::BindRepeating([] { return true; }));
  }

  // network_requests until |done| holds for its text, waiting between.
  std::string ListRequestsUntil(
      base::RepeatingCallback<bool(const std::string&)> done) {
    std::string list;
    for (int i = 0; i < 50; ++i) {
      list = Text(Run("network_requests"));
      if (done.Run(list)) {
        break;
      }
      Run("wait", R"({"seconds": 0.1})");
    }
    return list;
  }

  // Whether |script| comes to give |expected| in the page within a few
  // seconds (emulation reaches the renderer after DevTools answers).
  template <typename T>
  bool PageBecomes(const std::string& script, const T& expected) {
    for (int i = 0; i < 50; ++i) {
      if (content::EvalJs(contents(), script) == expected) {
        return true;
      }
      Run("wait", R"({"seconds": 0.1})");
    }
    ADD_FAILURE() << script << " never gave " << expected;
    return false;
  }

  // The id ("r3") of the request on the line of |list| containing |part|.
  static std::string RequestId(const std::string& list,
                               const std::string& part) {
    for (const std::string& line : base::SplitString(
             list, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      if (line.find(part) != std::string::npos && line[0] == 'r') {
        return line.substr(0, line.find(' '));
      }
    }
    ADD_FAILURE() << "No request with " << part << " in:\n" << list;
    return std::string();
  }

  std::unique_ptr<ClaudeTools> tools_;
};

IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, ReadsAndClicks) {
  ToolResult page = Run("read_page", R"({"filter": "interactive"})");
  EXPECT_FALSE(page.is_error) << Text(page);
  EXPECT_THAT(Text(page), HasSubstr("Title: Tools page"));
  EXPECT_THAT(Text(page), HasSubstr("Press me"));

  const std::string button = RefOf("Press me");
  ASSERT_FALSE(button.empty());
  ToolResult clicked = Run("click", R"({"ref": ")" + button + R"("})");
  EXPECT_FALSE(clicked.is_error) << Text(clicked);
  EXPECT_THAT(Text(clicked), HasSubstr("Clicked"));
  // A real renderer event, through DevTools, not a script's.
  EXPECT_EQ(content::EvalJs(contents(), "window.clicks"), 1);
  EXPECT_EQ(content::EvalJs(contents(), "window.trusted"), true);

  ToolResult found = Run("find", R"({"query": "agree"})");
  EXPECT_THAT(Text(found), HasSubstr("checkbox"));

  ToolResult js = Run("run_js", R"j({"code": "document.title + ' ' + (6 * 7)"})j");
  EXPECT_EQ(Text(js), "\"Tools page 42\"");
  ToolResult thrown = Run("run_js", R"j({"code": "nope()"})j");
  EXPECT_TRUE(thrown.is_error);
  EXPECT_THAT(Text(thrown), HasSubstr("nope is not defined"));
}

IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, TypesAndFillsForms) {
  const std::string name = RefOf("Your name");
  ToolResult typed = Run("type", R"({"ref": ")" + name + R"(", "text": "Ada"})");
  EXPECT_FALSE(typed.is_error) << Text(typed);
  EXPECT_EQ(content::EvalJs(contents(), "document.getElementById('name').value"),
            "Ada");

  ToolResult key = Run("key", R"({"keys": "Backspace"})");
  EXPECT_FALSE(key.is_error) << Text(key);
  EXPECT_EQ(content::EvalJs(contents(), "document.getElementById('name').value"),
            "Ad");
  ToolResult select_all = Run("key", R"({"keys": "cmd+a Backspace"})");
  EXPECT_FALSE(select_all.is_error) << Text(select_all);
#if BUILDFLAG(IS_MAC)
  EXPECT_EQ(content::EvalJs(contents(), "document.getElementById('name').value"),
            "");
#endif
  EXPECT_TRUE(Run("key", R"({"keys": "hyper+x"})").is_error);

  ToolResult color = Run(
      "form_input", R"({"ref": ")" + RefOf("Color") + R"(", "value": "Green"})");
  EXPECT_FALSE(color.is_error) << Text(color);
  EXPECT_EQ(content::EvalJs(contents(), "document.getElementById('color').value"),
            "g");
  ToolResult agree = Run(
      "form_input", R"({"ref": ")" + RefOf("I agree") + R"(", "value": true})");
  EXPECT_FALSE(agree.is_error) << Text(agree);
  EXPECT_EQ(
      content::EvalJs(contents(), "document.getElementById('agree').checked"),
      true);
}

IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, ReadsTheConsole) {
  // Logged before Claude attached: DevTools replays it.
  ToolResult console = Run("read_console");
  EXPECT_FALSE(console.is_error);
  EXPECT_THAT(Text(console), HasSubstr("[log] loaded 42"));
  EXPECT_THAT(Text(console), HasSubstr("boom"));
  // Read already.
  EXPECT_EQ(Text(Run("read_console")), "No new console messages.");

  ASSERT_TRUE(content::ExecJs(contents(),
                              "console.warn('careful'); console.log('fine');"));
  ToolResult warnings = Run("read_console", R"({"levels": ["warning"]})");
  EXPECT_THAT(Text(warnings), HasSubstr("[warning] careful"));
  EXPECT_THAT(Text(warnings), ::testing::Not(HasSubstr("fine")));
  ToolResult all =
      Run("read_console", R"({"only_new": false, "pattern": "car+eful"})");
  EXPECT_THAT(Text(all), HasSubstr("careful"));
  EXPECT_TRUE(Run("read_console", R"({"pattern": "("})").is_error);
}

IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, ScreenshotsAndCoordinates) {
  ToolResult shot = Run("screenshot");
  ASSERT_FALSE(shot.is_error) << Text(shot);
  ASSERT_EQ(shot.content.size(), 2u);
  const base::DictValue& image = shot.content[0].GetDict();
  EXPECT_EQ(*image.FindString("type"), "image");
  EXPECT_EQ(*image.FindStringByDottedPath("source.media_type"), "image/jpeg");
  EXPECT_GT(image.FindStringByDottedPath("source.data")->size(), 1000u);
  EXPECT_THAT(Text(shot), HasSubstr("Screenshot of the visible page"));

  // A click at the button's point in the screenshot (the test window is
  // small enough that one screenshot pixel is one CSS pixel).
  const int x =
      content::EvalJs(contents(),
                      "Math.round(document.getElementById('press')"
                      ".getBoundingClientRect().x + 5)")
          .ExtractInt();
  const int y =
      content::EvalJs(contents(),
                      "Math.round(document.getElementById('press')"
                      ".getBoundingClientRect().y + 5)")
          .ExtractInt();
  ToolResult clicked = Run("click", "{\"coordinate\": [" +
                                        base::NumberToString(x) + ", " +
                                        base::NumberToString(y) + "]}");
  EXPECT_FALSE(clicked.is_error) << Text(clicked);
  EXPECT_EQ(content::EvalJs(contents(), "window.clicks"), 1);

  ToolResult scrolled = Run("scroll", R"({"direction": "down"})");
  EXPECT_FALSE(scrolled.is_error) << Text(scrolled);
}

IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, WorksInABackgroundTab) {
  content::WebContents* target = contents();
  // The user opens another tab; Claude keeps working on its own.
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), embedded_test_server()->GetURL("/other.html"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_NE(contents(), target);

  ToolResult clicked =
      Run("click", R"({"ref": ")" + RefOf("Press me") + R"("})");
  EXPECT_FALSE(clicked.is_error) << Text(clicked);
  EXPECT_EQ(content::EvalJs(target, "window.clicks"), 1);
  ToolResult shot = Run("screenshot");
  EXPECT_FALSE(shot.is_error) << Text(shot);
}

IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, NavigatesAndRefusesBrowserPages) {
  ToolResult loaded =
      Run("navigate", "{\"url\": \"" +
                          embedded_test_server()->GetURL("/other.html").spec() +
                          "\"}");
  EXPECT_FALSE(loaded.is_error) << Text(loaded);
  EXPECT_THAT(Text(loaded), HasSubstr("Other"));
  ToolResult back = Run("navigate", R"({"url": "back"})");
  EXPECT_FALSE(back.is_error) << Text(back);
  EXPECT_EQ(contents()->GetLastCommittedURL().path(), "/tools.html");

  EXPECT_TRUE(Run("navigate", R"({"url": "chrome://settings"})").is_error);
  EXPECT_TRUE(Run("navigate", R"j({"url": "javascript:alert(1)"})j").is_error);

  // On a browser page, every tool refuses.
  ASSERT_TRUE(
      ui_test_utils::NavigateToURL(browser(), GURL("chrome://version")));
  ToolResult refused = Run("read_page");
  EXPECT_TRUE(refused.is_error);
  EXPECT_THAT(Text(refused), HasSubstr("can't use this page"));
  EXPECT_TRUE(Run("run_js", R"({"code": "1"})").is_error);

}

// The sidebar's tint (ClaudeActivity): on while tools run, a moment after,
// moved by select_tab, off with Stop.
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, MarksTheTabItActsIn) {
  content::WebContents* chat_tab = contents();
  EXPECT_FALSE(ClaudeActivity::IsActing(chat_tab));
  base::test::TestFuture<ToolResult> waiting;
  tools_->Run("wait", base::DictValue().Set("seconds", 0.3),
              waiting.GetCallback());
  EXPECT_TRUE(ClaudeActivity::IsActing(chat_tab));
  ASSERT_TRUE(waiting.Wait());
  // Lingers, for the next step.
  EXPECT_TRUE(ClaudeActivity::IsActing(chat_tab));

  SetBrowserScope(true);
  const std::string url = embedded_test_server()->GetURL("/other.html").spec();
  ToolResult opened = Run("open_tab", "{\"url\": \"" + url + "\"}");
  ASSERT_FALSE(opened.is_error) << Text(opened);
  content::WebContents* opened_tab = tools_->target();
  ASSERT_NE(opened_tab, chat_tab);
  EXPECT_FALSE(ClaudeActivity::IsActing(chat_tab));
  EXPECT_TRUE(ClaudeActivity::IsActing(opened_tab));

  tools_->Stop();
  EXPECT_FALSE(ClaudeActivity::IsActing(opened_tab));
  EXPECT_FALSE(ClaudeActivity::IsActing(chat_tab));

  // And it goes away by itself after the last step.
  ASSERT_FALSE(Run("get_page_text").is_error);
  EXPECT_TRUE(ClaudeActivity::IsActing(opened_tab));
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return !ClaudeActivity::IsActing(opened_tab); }));
}

IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, StopEndsWhatRuns) {
  base::test::TestFuture<ToolResult> result;
  tools_->Run("wait", base::DictValue().Set("seconds", 5),
              result.GetCallback());
  tools_->Stop();
  ASSERT_TRUE(result.IsReady());
  EXPECT_TRUE(result.Get().is_error);
  EXPECT_THAT(Text(result.Get()), HasSubstr("Stopped"));
  // It works again after.
  EXPECT_FALSE(Run("get_page_text").is_error);
}

IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, TabClosed) {
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), embedded_test_server()->GetURL("/other.html"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  tools_->SetTarget(contents());
  EXPECT_FALSE(Run("get_page_text").is_error);
  browser()->tab_strip_model()->CloseWebContentsAt(
      browser()->tab_strip_model()->active_index(),
      TabCloseTypes::CLOSE_NONE);
  ASSERT_TRUE(base::test::RunUntil([&] { return !tools_->target(); }));
  ToolResult closed = Run("read_page");
  EXPECT_TRUE(closed.is_error);
  EXPECT_THAT(Text(closed), HasSubstr("closed"));
}

// docs/design/claude.md, D5: the browser tools.
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, BrowserToolsNeedThePermission) {
  const std::string url = embedded_test_server()->GetURL("/other.html").spec();
  ToolResult listed = Run("list_tabs");
  EXPECT_TRUE(listed.is_error);
  EXPECT_THAT(Text(listed), HasSubstr("Let Claude use the whole browser"));
  EXPECT_TRUE(Run("open_tab", "{\"url\": \"" + url + "\"}").is_error);
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
  // The page tools work on the chat's own tab.
  EXPECT_FALSE(Run("get_page_text").is_error);
}

IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, ListsOpensSelectsAndCloses) {
  SetBrowserScope(true);
  content::WebContents* chat_tab = contents();
  const std::string chat_id = IdOf(chat_tab);

  ToolResult listed = Run("list_tabs");
  ASSERT_FALSE(listed.is_error) << Text(listed);
  EXPECT_THAT(Text(listed), HasSubstr("[shown in the user's window]"));
  EXPECT_THAT(Text(listed),
              HasSubstr(chat_id + ": Tools page — " +
                        chat_tab->GetVisibleURL().spec()));
  EXPECT_THAT(Text(listed), HasSubstr("[front] [you act on this]"));

  // In the background: the user's tab stays in front.
  const std::string url = embedded_test_server()->GetURL("/other.html").spec();
  ToolResult opened = Run("open_tab", "{\"url\": \"" + url + "\"}");
  ASSERT_FALSE(opened.is_error) << Text(opened);
  const std::string new_id = OpenedId(opened);
  ASSERT_FALSE(new_id.empty()) << Text(opened);
  EXPECT_EQ(browser()->tab_strip_model()->count(), 2);
  EXPECT_EQ(contents(), chat_tab);
  // The page tools act on the new tab.
  EXPECT_THAT(Text(Run("get_page_text")), HasSubstr("Other page"));
  EXPECT_THAT(Text(Run("list_tabs")),
              HasSubstr(new_id + ": Other — " + url +
                        " [you act on this] [you opened it]"));

  ToolResult selected =
      Run("select_tab", "{\"tab_id\": \"" + chat_id + "\"}");
  ASSERT_FALSE(selected.is_error) << Text(selected);
  EXPECT_THAT(Text(Run("get_page_text")), HasSubstr("Title: Tools page"));
  EXPECT_TRUE(Run("select_tab", R"({"tab_id": "t999999"})").is_error);

  // Any tab closes (D7); Claude's prompt asks it to check with the user.
  ToolResult closed = Run("close_tab", "{\"tab_id\": \"" + new_id + "\"}");
  EXPECT_FALSE(closed.is_error) << Text(closed);
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return browser()->tab_strip_model()->count() == 1; }));
}

IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, PermissionTurnedOffMidTask) {
  SetBrowserScope(true);
  const std::string url = embedded_test_server()->GetURL("/other.html").spec();
  ASSERT_FALSE(Run("open_tab", "{\"url\": \"" + url + "\"}").is_error);
  SetBrowserScope(false);
  // The opened tab isn't the chat's: refused now.
  ToolResult refused = Run("get_page_text");
  EXPECT_TRUE(refused.is_error);
  EXPECT_THAT(Text(refused), HasSubstr("Not permitted"));
  // The next message starts on the chat's tab again.
  tools_->SetTarget(contents());
  EXPECT_FALSE(Run("get_page_text").is_error);
}

// Tabs of a space no window shows live in the hidden parking window: listed
// as not shown, and usable.
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, TabsOfHiddenSpaces) {
  SetBrowserScope(true);
  content::WebContents* tools_tab = contents();
  const std::string tools_id = IdOf(tools_tab);
  WindowFeatures* features = WindowFeatures::From(browser());
  ASSERT_TRUE(features);
  const std::string home_name =
      base::UTF16ToUTF8(features->space_model().name());
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");
  features->SwitchToSpace(*work);
  ASSERT_NE(contents(), tools_tab);

  ToolResult listed = Run("list_tabs");
  ASSERT_FALSE(listed.is_error) << Text(listed);
  EXPECT_THAT(Text(listed),
              HasSubstr("Space \"Work\" (s"));
  EXPECT_THAT(Text(listed), HasSubstr("[shown in the user's window]"));
  EXPECT_THAT(Text(listed), HasSubstr("Space \"" + home_name + "\" (s"));
  EXPECT_THAT(Text(listed), HasSubstr(tools_id + ": Tools page"));

  ASSERT_FALSE(
      Run("select_tab", "{\"tab_id\": \"" + tools_id + "\"}").is_error);
  EXPECT_THAT(Text(Run("get_page_text")), HasSubstr("Title: Tools page"));
  EXPECT_FALSE(Run("run_js", R"({"code": "1 + 1"})").is_error);
}

// docs/design/claude.md, D7: the sidebar's own features.
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, OrganizesTheSidebar) {
  SetBrowserScope(true);
  SpaceModel& space = WindowFeatures::From(browser())->space_model();
  const std::string chat_id = IdOf(contents());
  const std::string url = embedded_test_server()->GetURL("/other.html").spec();
  const std::string other_id =
      OpenedId(Run("open_tab", "{\"url\": \"" + url + "\"}"));
  ASSERT_FALSE(other_id.empty());
  auto in = [&](const SidebarNode* root, const std::string& id) {
    return Text(Run("list_tabs")).find(id) != std::string::npos &&
           std::ranges::any_of(root->children(), [&](const auto& child) {
             return "n" + child->id().AsLowercaseString().substr(0, 8) == id;
           });
  };

  // Pin, then unpin; reorder.
  ASSERT_FALSE(Run("move_tab", "{\"tab_id\": \"" + chat_id +
                                   "\", \"to\": \"pinned\"}")
                   .is_error);
  EXPECT_TRUE(in(space.pinned_root(), chat_id));
  ASSERT_FALSE(Run("move_tab", "{\"tab_id\": \"" + chat_id +
                                   "\", \"to\": \"today\", \"index\": 0}")
                   .is_error);
  EXPECT_TRUE(in(space.today_root(), chat_id));
  EXPECT_EQ("n" + space.today_root()->children()[0]->id()
                      .AsLowercaseString()
                      .substr(0, 8),
            chat_id);

  // A favorite, and back.
  SpaceModel& favorites = WindowFeatures::From(browser())
                              ->service()
                              .space_list()
                              .favorites();
  ASSERT_FALSE(Run("move_tab", "{\"tab_id\": \"" + other_id +
                                   "\", \"to\": \"favorites\"}")
                   .is_error);
  EXPECT_TRUE(in(favorites.pinned_root(), other_id));
  EXPECT_THAT(Text(Run("list_tabs")), HasSubstr("Favorites (every space)"));
  ASSERT_FALSE(Run("move_tab", "{\"tab_id\": \"" + other_id +
                                   "\", \"to\": \"today\"}")
                   .is_error);
  EXPECT_TRUE(in(space.today_root(), other_id));

  // A folder, and a tab in it, renamed.
  ToolResult folder = Run("create_folder", R"({"name": "Reading"})");
  ASSERT_FALSE(folder.is_error) << Text(folder);
  const std::string folder_text = Text(folder);
  const std::string folder_id =
      folder_text.substr(folder_text.find("folder_id ") + 10, 9);
  ASSERT_FALSE(Run("move_tab", "{\"tab_id\": \"" + other_id +
                                   "\", \"to\": \"folder\", \"folder_id\": \"" +
                                   folder_id + "\"}")
                   .is_error);
  ASSERT_FALSE(Run("rename_tab", "{\"tab_id\": \"" + other_id +
                                     "\", \"title\": \"Later\"}")
                   .is_error);
  const std::string listed = Text(Run("list_tabs"));
  EXPECT_THAT(listed, HasSubstr("folder \"Reading\" (folder_id " + folder_id +
                                "):\n      - " + other_id + ": Later"));
  // Folders don't go to Today.
  EXPECT_TRUE(Run("move_tab", "{\"tab_id\": \"" + folder_id +
                                  "\", \"to\": \"today\"}")
                  .is_error);

  // Mute; show it to the user, which moves Claude to it.
  ASSERT_FALSE(Run("mute_tab", "{\"tab_id\": \"" + other_id +
                                   "\", \"muted\": true}")
                   .is_error);
  EXPECT_THAT(Text(Run("list_tabs")), HasSubstr("Later — " + url));
  ASSERT_FALSE(
      Run("activate_tab", "{\"tab_id\": \"" + other_id + "\"}").is_error);
  EXPECT_EQ(contents()->GetVisibleURL().spec(), url);
  EXPECT_TRUE(contents()->IsAudioMuted());
  EXPECT_EQ(tools_->target(), contents());
}

IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, SpacesAndUnloadedEntries) {
  SetBrowserScope(true);
  const std::string chat_id = IdOf(contents());
  // Pinned and closed: it stays, unloaded.
  ASSERT_FALSE(Run("move_tab", "{\"tab_id\": \"" + chat_id +
                                   "\", \"to\": \"pinned\"}")
                   .is_error);
  const std::string url = embedded_test_server()->GetURL("/other.html").spec();
  ASSERT_FALSE(Run("open_tab", "{\"url\": \"" + url + "\", \"active\": true}")
                   .is_error);
  ToolResult closed = Run("close_tab", "{\"tab_id\": \"" + chat_id + "\"}");
  ASSERT_FALSE(closed.is_error) << Text(closed);
  EXPECT_TRUE(base::test::RunUntil([&] {
    return Text(Run("list_tabs")).find(chat_id + ": Tools page") !=
               std::string::npos &&
           Text(Run("list_tabs")).find("[unloaded]") != std::string::npos;
  })) << Text(Run("list_tabs"));
  // select_tab loads it, in the background.
  content::WebContents* front = contents();
  ToolResult selected = Run("select_tab", "{\"tab_id\": \"" + chat_id + "\"}");
  ASSERT_FALSE(selected.is_error) << Text(selected);
  EXPECT_EQ(contents(), front);
  EXPECT_TRUE(base::test::RunUntil([&] {
    return Text(Run("get_page_text")).find("Title: Tools page") !=
           std::string::npos;
  }));

  // Spaces.
  ToolResult created = Run("create_space", R"({"name": "Errands"})");
  ASSERT_FALSE(created.is_error) << Text(created);
  ASSERT_FALSE(Run("switch_space", R"({"space": "errands"})").is_error);
  EXPECT_EQ(WindowFeatures::From(browser())->space_model().name(),
            u"Errands");
  EXPECT_TRUE(Run("switch_space", R"({"space": "Nowhere"})").is_error);
  // A tab moves to another space: here, into Errands' Pinned, from the
  // window that showed its old space.
  SpaceModel* errands = &WindowFeatures::From(browser())->space_model();
  const std::string moved_id = IdOf(front);
  ToolResult moved = Run("move_tab", "{\"tab_id\": \"" + moved_id +
                                         "\", \"to\": \"pinned\", "
                                         "\"space\": \"Errands\"}");
  ASSERT_FALSE(moved.is_error) << Text(moved);
  EXPECT_TRUE(std::ranges::any_of(
      errands->pinned_root()->children(), [&](const auto& child) {
        return "n" + child->id().AsLowercaseString().substr(0, 8) ==
               moved_id;
      }));
  // The entry of a space no window shows doesn't load in the background,
  // but activate_tab shows it.
  EXPECT_TRUE(Run("move_tab", "{\"tab_id\": \"" + chat_id +
                                  "\", \"to\": \"nowhere\"}")
                  .is_error);
  ASSERT_FALSE(
      Run("activate_tab", "{\"tab_id\": \"" + chat_id + "\"}").is_error);
  EXPECT_NE(WindowFeatures::From(browser())->space_model().name(),
            u"Errands");
  EXPECT_EQ(contents()->GetVisibleURL().path(), "/tools.html");
}

IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, HistoryDownloadsDevices) {
  SetBrowserScope(true);
  // The visit to the test page is in the history, in a moment.
  EXPECT_TRUE(base::test::RunUntil([&] {
    return Text(Run("search_history", R"({"query": "Tools page"})"))
               .find("tools.html") != std::string::npos;
  }));
  EXPECT_THAT(Text(Run("search_history", R"({"query": "zebra-nothing"})")),
              HasSubstr("No matching history."));
  EXPECT_EQ(Text(Run("list_downloads")), "No downloads.");
  // No sync in tests.
  EXPECT_TRUE(Run("send_tab_to_device").is_error);
}

// The file tools refuse without the setting, keep to the folder, and write
// only what the user allowed (docs/design/claude.md, "Files").
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, FilesNeedTheSetting) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath folder = dir.GetPath().AppendASCII("Work");
  ASSERT_TRUE(base::CreateDirectory(folder));

  FilesConfig config;
  std::vector<ApprovalRequest> asked;
  bool allow = true;
  tools_->SetFiles(
      base::BindLambdaForTesting([&] { return config; }),
      base::BindLambdaForTesting(
          [&](ApprovalRequest request, base::OnceCallback<void(bool)> done) {
            asked.push_back(std::move(request));
            std::move(done).Run(allow);
          }));

  // Off.
  ToolResult off = Run("save_file", R"({"name":"a.md","content":"x"})");
  EXPECT_TRUE(off.is_error);
  EXPECT_NE(Text(off).find("Not permitted"), std::string::npos);
  EXPECT_TRUE(asked.empty());

  // Save files: new files only, each asked about.
  config.access = prefs::FilesAccess::kSave;
  config.folder = folder;
  EXPECT_TRUE(Run("list_files").is_error);
  EXPECT_TRUE(Run("read_file", R"({"path":"a.md"})").is_error);
  ToolResult saved = Run("save_file", R"({"name":"a.md","content":"one"})");
  EXPECT_FALSE(saved.is_error) << Text(saved);
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0].tool, "save_file");
  EXPECT_EQ(asked[0].title, "Save \u201ca.md\u201d in Work");
  EXPECT_EQ(asked[0].detail, "one");
  EXPECT_FALSE(Run("save_file", R"({"name":"a.md","content":"two"})").is_error);
  std::string text;
  ASSERT_TRUE(base::ReadFileToString(folder.AppendASCII("a.md"), &text));
  EXPECT_EQ(text, "one");
  ASSERT_TRUE(base::ReadFileToString(folder.AppendASCII("a (1).md"), &text));
  EXPECT_EQ(text, "two");
  EXPECT_TRUE(Run("save_file", R"({"name":"../x.md","content":"x"})").is_error);

  // Use a folder: write, replace, read, list; nothing outside.
  config.access = prefs::FilesAccess::kFolder;
  asked.clear();
  EXPECT_FALSE(
      Run("write_file", R"({"path":"notes/b.md","content":"hi"})").is_error);
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0].title, "Create \u201cnotes/b.md\u201d in Work");
  EXPECT_FALSE(
      Run("write_file", R"({"path":"notes/b.md","content":"hello"})").is_error);
  ASSERT_EQ(asked.size(), 2u);
  EXPECT_EQ(asked[1].title, "Replace \u201cnotes/b.md\u201d in Work");
  EXPECT_EQ(Text(Run("read_file", R"({"path":"notes/b.md"})")), "hello");
  EXPECT_NE(Text(Run("list_files")).find("notes/"), std::string::npos);
  EXPECT_TRUE(
      Run("write_file", R"({"path":"../out.md","content":"x"})").is_error);
  EXPECT_FALSE(base::PathExists(dir.GetPath().AppendASCII("out.md")));
  EXPECT_EQ(asked.size(), 2u);

  // Denied: nothing written.
  allow = false;
  ToolResult denied =
      Run("write_file", R"({"path":"denied.md","content":"x"})");
  EXPECT_TRUE(denied.is_error);
  EXPECT_EQ(Text(denied), "The user didn't allow it.");
  EXPECT_FALSE(base::PathExists(folder.AppendASCII("denied.md")));

  // "Use a folder" without one.
  config.folder.clear();
  EXPECT_NE(Text(Run("list_files")).find("hasn't chosen a folder"),
            std::string::npos);
}

// Stop while the user decides: the tool ends, and a late answer writes
// nothing.
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, StopWhileAsking) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  FilesConfig config;
  config.access = prefs::FilesAccess::kFolder;
  config.folder = dir.GetPath();
  base::OnceCallback<void(bool)> answer;
  tools_->SetFiles(
      base::BindLambdaForTesting([&] { return config; }),
      base::BindLambdaForTesting(
          [&](ApprovalRequest request, base::OnceCallback<void(bool)> done) {
            answer = std::move(done);
          }));
  base::test::TestFuture<ToolResult> result;
  base::DictValue input;
  input.Set("path", "late.md");
  input.Set("content", "x");
  tools_->Run("write_file", std::move(input), result.GetCallback());
  ASSERT_TRUE(base::test::RunUntil([&] { return !answer.is_null(); }));
  tools_->Stop();
  EXPECT_EQ(Text(result.Get()), "Stopped by the user.");
  std::move(answer).Run(true);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(base::PathExists(dir.GetPath().AppendASCII("late.md")));
}

// The developer tools are sent only with their setting, and refuse
// without it, also when it's turned off mid-task (docs/design/claude.md,
// "Developer tools").
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, DevToolsNeedTheSetting) {
  auto has = [](const base::ListValue& definitions, std::string_view name) {
    return std::ranges::any_of(definitions, [&](const base::Value& tool) {
      const std::string* tool_name = tool.GetDict().FindString("name");
      return tool_name && *tool_name == name;
    });
  };
  EXPECT_FALSE(has(ClaudeTools::Definitions(), "network_requests"));
  EXPECT_TRUE(has(ClaudeTools::Definitions(/*memory=*/false, /*files=*/true,
                                           /*devtools=*/true),
                  "network_requests"));
  // Not given here: refused.
  EXPECT_TRUE(Run("network_requests").is_error);

  bool on = false;
  tools_->SetDevTools(base::BindLambdaForTesting([&] { return on; }));
  ToolResult off = Run("network_requests");
  EXPECT_TRUE(off.is_error);
  EXPECT_THAT(Text(off), HasSubstr("Let Claude use developer tools"));
  on = true;
  EXPECT_FALSE(Run("network_requests").is_error);
  on = false;
  EXPECT_TRUE(Run("network_response", R"({"id": "r1"})").is_error);
  // The page tools don't need it.
  EXPECT_FALSE(Run("read_console").is_error);
}

// Requests from the first tool on, with their headers and bodies; secrets
// hidden unless asked for.
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, NetworkRequests) {
  AllowDevTools();
  EXPECT_THAT(Text(Run("network_requests")),
              HasSubstr("No requests recorded"));
  ASSERT_TRUE(content::ExecJs(contents(), R"(
      Promise.all([
        fetch('/api.json?access_token=abc',
              {headers: {'Authorization': 'Bearer secret1'}})
            .then(r => r.text()),
        fetch('/missing').then(r => r.status)]))"));
  const std::string list = ListRequestsUntil(
      base::BindRepeating([](const std::string& list) {
        return list.find("/missing") != std::string::npos &&
               list.find("api.json") != std::string::npos &&
               list.find("pending") == std::string::npos;
      }));
  EXPECT_THAT(list, HasSubstr("GET 200 Fetch"));
  EXPECT_THAT(list, HasSubstr("api.json?access_token=<hidden>"));
  EXPECT_THAT(list, HasSubstr("GET 404 Fetch"));
  EXPECT_THAT(list, ::testing::Not(HasSubstr("access_token=abc")));

  const std::string failed =
      Text(Run("network_requests", R"({"failed": true})"));
  EXPECT_THAT(failed, HasSubstr("/missing"));
  EXPECT_THAT(failed, ::testing::Not(HasSubstr("api.json")));
  EXPECT_EQ(Text(Run("network_requests", R"({"only_new": true})")),
            "No new requests.");
  EXPECT_THAT(Text(Run("network_requests", R"({"url": "miss+ing"})")),
              ::testing::Not(HasSubstr("api.json")));
  EXPECT_TRUE(Run("network_requests", R"({"url": "("})").is_error);

  const std::string id = RequestId(list, "api.json");
  ToolResult response =
      Run("network_response", R"({"id": ")" + id + R"("})");
  ASSERT_FALSE(response.is_error) << Text(response);
  EXPECT_THAT(Text(response), HasSubstr("Status: 200"));
  EXPECT_THAT(Text(response), HasSubstr("authorization: <hidden"));
  EXPECT_THAT(Text(response), HasSubstr(R"("name":"Ada")"));
  EXPECT_THAT(Text(response), HasSubstr(R"("token":"<hidden>")"));
  EXPECT_THAT(Text(response), ::testing::Not(HasSubstr("secret1")));
  EXPECT_THAT(Text(response), ::testing::Not(HasSubstr("t0ps3cret")));

  const std::string shown = Text(Run(
      "network_response", R"({"id": ")" + id + R"(", "show_secrets": true})"));
  EXPECT_THAT(shown, HasSubstr("Bearer secret1"));
  EXPECT_THAT(shown, HasSubstr("t0ps3cret"));
  EXPECT_THAT(shown, HasSubstr("access_token=abc"));

  EXPECT_TRUE(Run("network_response", R"({"id": "r999"})").is_error);
}

// An element's box, styles with their rules, and listeners; by selector or
// by ref.
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, InspectElement) {
  AllowDevTools();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/styled.html")));
  ToolResult box = Run("inspect_element", R"({"selector": "#box"})");
  ASSERT_FALSE(box.is_error) << Text(box);
  const std::string text = Text(box);
  EXPECT_THAT(text, HasSubstr(R"(<div id="box" class="card">)"));
  EXPECT_THAT(text, HasSubstr("Box: 200×50"));
  EXPECT_THAT(text, HasSubstr("padding 10 10 10 10"));
  EXPECT_THAT(text, HasSubstr("display: flex"));
  EXPECT_THAT(text, HasSubstr("color: rgb(255, 0, 0)"));
  // The winning rule first, from the page's <style>.
  const size_t id_rule = text.find("  #box");
  const size_t class_rule = text.find("  .card");
  ASSERT_NE(id_rule, std::string::npos) << text;
  ASSERT_NE(class_rule, std::string::npos) << text;
  EXPECT_LT(id_rule, class_rule);
  EXPECT_THAT(text, HasSubstr("<style> in "));
  EXPECT_THAT(text, HasSubstr("    width: 180px;"));
  EXPECT_THAT(text, HasSubstr("click (passive)"));
  EXPECT_THAT(text, HasSubstr("boxClicked"));

  ToolResult go = Run("inspect_element",
                      R"({"ref": ")" + RefOf("Go") + R"(", "properties": ["margin-top"]})");
  ASSERT_FALSE(go.is_error) << Text(go);
  EXPECT_THAT(Text(go), HasSubstr("<button"));
  EXPECT_THAT(Text(go), HasSubstr("margin-top: 4px"));
  EXPECT_THAT(Text(go), HasSubstr("element.style"));
  EXPECT_THAT(Text(go), ::testing::Not(HasSubstr("display:")));

  EXPECT_TRUE(Run("inspect_element", R"({"selector": "#nope"})").is_error);
  EXPECT_TRUE(Run("inspect_element").is_error);
}

// Cookies, localStorage and sessionStorage; cookie values and
// credential-like ones hidden unless asked for.
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, PageStorage) {
  AllowDevTools();
  ASSERT_TRUE(content::ExecJs(contents(), R"(
      document.cookie = 'theme=dark';
      document.cookie = 'session_id=abc123';
      localStorage.setItem('cart', '3 items');
      localStorage.setItem('auth_token', 'xyz789');
      sessionStorage.setItem('step', '2');)"));
  ToolResult storage = Run("page_storage");
  ASSERT_FALSE(storage.is_error) << Text(storage);
  const std::string text = Text(storage);
  EXPECT_THAT(text, HasSubstr("Cookies (2)"));
  EXPECT_THAT(text, HasSubstr("theme=<hidden, 4 characters>"));
  EXPECT_THAT(text, HasSubstr("cart = 3 items"));
  EXPECT_THAT(text, HasSubstr("auth_token = <hidden, 6 characters>"));
  EXPECT_THAT(text, HasSubstr("step = 2"));
  EXPECT_THAT(text, HasSubstr("IndexedDB"));
  EXPECT_THAT(text, ::testing::Not(HasSubstr("abc123")));
  EXPECT_THAT(text, ::testing::Not(HasSubstr("xyz789")));

  const std::string shown = Text(Run("page_storage", R"({"show_values": true})"));
  EXPECT_THAT(shown, HasSubstr("theme=dark"));
  EXPECT_THAT(shown, HasSubstr("abc123"));
  EXPECT_THAT(shown, HasSubstr("xyz789"));

  const std::string cart =
      Text(Run("page_storage", R"({"store": "localStorage", "filter": "CART"})"));
  EXPECT_THAT(cart, HasSubstr("cart = 3 items"));
  EXPECT_THAT(cart, ::testing::Not(HasSubstr("auth_token")));
  EXPECT_THAT(cart, ::testing::Not(HasSubstr("Cookies")));
}

// Each change is asked about first; a denied one changes nothing.
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, EditPageStorage) {
  AllowDevTools();
  std::vector<ApprovalRequest> asked;
  bool allow = true;
  tools_->SetFiles(
      base::BindRepeating([] { return FilesConfig(); }),
      base::BindLambdaForTesting(
          [&](ApprovalRequest request, base::OnceCallback<void(bool)> done) {
            asked.push_back(std::move(request));
            std::move(done).Run(allow);
          }));
  ASSERT_TRUE(content::ExecJs(contents(),
                              "localStorage.setItem('cart', '3 items');"
                              "sessionStorage.setItem('step', '2');"));

  ToolResult set = Run("edit_page_storage", R"({"store": "localStorage",
      "action": "set", "name": "cart", "value": "4 items"})");
  ASSERT_FALSE(set.is_error) << Text(set);
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0].tool, "edit_page_storage");
  EXPECT_THAT(asked[0].title, HasSubstr("“cart”"));
  EXPECT_EQ(asked[0].detail, "4 items");
  EXPECT_EQ(content::EvalJs(contents(), "localStorage.getItem('cart')"),
            "4 items");

  allow = false;
  ToolResult denied = Run("edit_page_storage",
      R"({"store": "localStorage", "action": "remove", "name": "cart"})");
  EXPECT_TRUE(denied.is_error);
  EXPECT_EQ(Text(denied), "The user didn't allow it.");
  EXPECT_EQ(content::EvalJs(contents(), "localStorage.getItem('cart')"),
            "4 items");

  allow = true;
  ToolResult cookie = Run("edit_page_storage",
      R"({"store": "cookies", "action": "set", "name": "lang", "value": "it"})");
  ASSERT_FALSE(cookie.is_error) << Text(cookie);
  EXPECT_THAT(content::EvalJs(contents(), "document.cookie").ExtractString(),
              HasSubstr("lang=it"));
  ToolResult removed = Run("edit_page_storage",
      R"({"store": "cookies", "action": "remove", "name": "lang"})");
  ASSERT_FALSE(removed.is_error) << Text(removed);
  EXPECT_THAT(content::EvalJs(contents(), "document.cookie").ExtractString(),
              ::testing::Not(HasSubstr("lang=it")));

  ToolResult cleared = Run("edit_page_storage",
      R"({"store": "sessionStorage", "action": "clear"})");
  ASSERT_FALSE(cleared.is_error) << Text(cleared);
  EXPECT_EQ(content::EvalJs(contents(), "sessionStorage.length"), 0);

  // Bad input isn't asked about.
  const size_t before = asked.size();
  EXPECT_TRUE(Run("edit_page_storage",
                  R"({"store": "cookies", "action": "set"})").is_error);
  EXPECT_EQ(asked.size(), before);
}

// Load timings, vitals and runtime numbers.
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, Performance) {
  AllowDevTools();
  ToolResult performance = Run("performance");
  ASSERT_FALSE(performance.is_error) << Text(performance);
  const std::string text = Text(performance);
  EXPECT_THAT(text, HasSubstr("Performance of "));
  EXPECT_THAT(text, HasSubstr("tools.html"));
  EXPECT_THAT(text, HasSubstr("First contentful paint"));
  EXPECT_THAT(text, HasSubstr("Cumulative layout shift"));
  EXPECT_THAT(text, HasSubstr("JavaScript heap"));
  EXPECT_THAT(text, HasSubstr("DOM nodes"));
}

// Emulation adds up across calls, and is undone by reset and by Stop().
IN_PROC_BROWSER_TEST_F(ClaudeToolsBrowserTest, EmulateIsUndone) {
  AllowDevTools();
  const int width = content::EvalJs(contents(), "innerWidth").ExtractInt();
  const bool was_dark =
      content::EvalJs(contents(),
                      "matchMedia('(prefers-color-scheme: dark)').matches")
          .ExtractBool();
  const std::string other = was_dark ? "light" : "dark";
  constexpr char kDark[] = "matchMedia('(prefers-color-scheme: dark)').matches";

  ToolResult on = Run("emulate", R"({"width": 500, "height": 700,
      "color_scheme": ")" + other + R"("})");
  ASSERT_FALSE(on.is_error) << Text(on);
  EXPECT_THAT(Text(on), HasSubstr("color scheme: " + other));
  EXPECT_THAT(Text(on), HasSubstr("500×700"));
  EXPECT_TRUE(PageBecomes("innerWidth", 500));
  EXPECT_TRUE(PageBecomes(kDark, !was_dark));

  // Print media keeps the color scheme.
  ToolResult print = Run("emulate", R"({"media": "print"})");
  ASSERT_FALSE(print.is_error) << Text(print);
  EXPECT_TRUE(PageBecomes("matchMedia('print').matches", true));
  EXPECT_TRUE(PageBecomes(kDark, !was_dark));

  EXPECT_THAT(Text(Run("emulate", R"({"reset": true})")),
              HasSubstr("Emulation undone"));
  EXPECT_TRUE(PageBecomes("innerWidth", width));
  EXPECT_TRUE(PageBecomes(kDark, was_dark));
  EXPECT_TRUE(PageBecomes("matchMedia('print').matches", false));

  // Stopping lets go of the tab, which undoes it too.
  ASSERT_FALSE(Run("emulate", R"({"color_scheme": ")" + other + R"("})")
                   .is_error);
  EXPECT_TRUE(PageBecomes(kDark, !was_dark));
  tools_->Stop();
  EXPECT_TRUE(PageBecomes(kDark, was_dark));

  EXPECT_TRUE(Run("emulate").is_error);
  EXPECT_TRUE(Run("emulate", R"({"device": "toaster"})").is_error);
}

}  // namespace
}  // namespace compass::claude
