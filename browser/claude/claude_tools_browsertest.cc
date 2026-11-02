// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_tools.h"

#include <cstring>
#include <memory>
#include <string>

#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "build/build_config.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/claude/claude_prefs.h"
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
  EXPECT_THAT(Text(listed), HasSubstr("Window 1"));
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
                        " (space: " +
                        base::UTF16ToUTF8(WindowFeatures::From(browser())
                                              ->space_model()
                                              .name()) +
                        ") [you act on this] [you opened it]"));

  ToolResult selected =
      Run("select_tab", "{\"tab_id\": \"" + chat_id + "\"}");
  ASSERT_FALSE(selected.is_error) << Text(selected);
  EXPECT_THAT(Text(Run("get_page_text")), HasSubstr("Title: Tools page"));
  EXPECT_TRUE(Run("select_tab", R"({"tab_id": "t999999"})").is_error);

  // Only tabs Claude opened close.
  ToolResult refused =
      Run("close_tab", "{\"tab_id\": \"" + chat_id + "\"}");
  EXPECT_TRUE(refused.is_error);
  EXPECT_EQ(browser()->tab_strip_model()->count(), 2);
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
  EXPECT_THAT(Text(listed), HasSubstr("Window 1 (shows space Work)"));
  EXPECT_THAT(Text(listed), HasSubstr("Not shown (spaces no window shows):\n- " +
                                      tools_id + ": Tools page"));
  EXPECT_THAT(Text(listed), HasSubstr("(space: " + home_name + ")"));

  ASSERT_FALSE(
      Run("select_tab", "{\"tab_id\": \"" + tools_id + "\"}").is_error);
  EXPECT_THAT(Text(Run("get_page_text")), HasSubstr("Title: Tools page"));
  EXPECT_FALSE(Run("run_js", R"({"code": "1 + 1"})").is_error);
}

}  // namespace
}  // namespace compass::claude
