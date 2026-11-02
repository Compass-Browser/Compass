// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/find_bar/find_bar.h"

#include <string>

#include "base/test/run_until.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/find_bar/find_bar.h"
#include "chrome/browser/ui/find_bar/find_bar_controller.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace compass {
namespace {

constexpr char kPage[] =
    "data:text/html,<p>apple apple <span id=b>banana</span> apple</p>";

constexpr char kRoot[] =
    "document.querySelector('compass-find-bar-app').shadowRoot";

class FindBarBrowserTest : public InProcessBrowserTest {
 protected:
  FindBar* bar() { return FindBar::From(browser()); }

  content::WebContents* tab() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  // Cmd+F, until the bar shows with its page connected.
  content::WebContents* Open() {
    chrome::ExecuteCommand(browser(), IDC_FIND);
    EXPECT_TRUE(base::test::RunUntil([&] {
      return bar()->IsShowing() && bar()->IsPageConnectedForTesting();
    }));
    return bar()->page_contents();
  }

  std::string Count() {
    return content::EvalJs(bar()->page_contents(),
                           std::string(kRoot) +
                               ".querySelector('#count').textContent")
        .ExtractString();
  }

  std::string Field() {
    return content::EvalJs(bar()->page_contents(),
                           std::string(kRoot) + ".querySelector('#input').value")
        .ExtractString();
  }

  bool CountBecomes(const std::string& count) {
    return base::test::RunUntil([&] { return Count() == count; });
  }

  void Type(const std::string& text) {
    ASSERT_TRUE(content::ExecJs(
        bar()->page_contents(),
        content::JsReplace(std::string("const input = ") + kRoot +
                               ".querySelector('#input');"
                               "input.value = $1;"
                               "input.dispatchEvent(new InputEvent('input', "
                               "{inputType: 'insertText'}));",
                           text)));
  }

  void Key(const std::string& key, bool shift = false) {
    ASSERT_TRUE(content::ExecJs(
        bar()->page_contents(),
        content::JsReplace(std::string(kRoot) +
                               ".querySelector('#input').dispatchEvent("
                               "new KeyboardEvent('keydown', "
                               "{key: $1, shiftKey: $2}));",
                           key, shift)));
  }
};

IN_PROC_BROWSER_TEST_F(FindBarBrowserTest, FindsAndMovesBetweenMatches) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL(kPage)));
  ASSERT_TRUE(bar());
  EXPECT_FALSE(bar()->IsShowing());
  ASSERT_TRUE(Open());
  // Over the page, not empty.
  EXPECT_FALSE(bar()->bounds().IsEmpty());

  Type("apple");
  EXPECT_TRUE(CountBecomes("1/3"));
  Key("Enter");
  EXPECT_TRUE(CountBecomes("2/3"));
  Key("Enter", /*shift=*/true);
  EXPECT_TRUE(CountBecomes("1/3"));
  // Cmd+G and Cmd+Shift+G, from the window.
  chrome::ExecuteCommand(browser(), IDC_FIND_NEXT);
  EXPECT_TRUE(CountBecomes("2/3"));
  chrome::ExecuteCommand(browser(), IDC_FIND_PREVIOUS);
  EXPECT_TRUE(CountBecomes("1/3"));

  Type("cherry");
  EXPECT_TRUE(CountBecomes("No results"));
  Type("");
  EXPECT_TRUE(CountBecomes(""));

  Key("Escape");
  EXPECT_TRUE(base::test::RunUntil([&] { return !bar()->IsShowing(); }));
}

IN_PROC_BROWSER_TEST_F(FindBarBrowserTest, EachTabKeepsItsBar) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL(kPage)));
  ASSERT_TRUE(Open());
  Type("apple");
  ASSERT_TRUE(CountBecomes("1/3"));

  // Another tab: no bar.
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL(kPage), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  EXPECT_TRUE(base::test::RunUntil([&] { return !bar()->IsShowing(); }));

  // Back: the bar, with that tab's text and matches.
  browser()->tab_strip_model()->ActivateTabAt(0);
  EXPECT_TRUE(base::test::RunUntil([&] { return bar()->IsShowing(); }));
  EXPECT_EQ(Field(), "apple");
  EXPECT_TRUE(CountBecomes("1/3"));
}

IN_PROC_BROWSER_TEST_F(FindBarBrowserTest, StartsFromTheSelection) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL(kPage)));
  // Headless, the page never has focus, so only the page itself knows its
  // selection: the bar asks it.
  ASSERT_TRUE(content::ExecJs(tab(),
                              "getSelection().selectAllChildren("
                              "document.getElementById('b'));"));
  ASSERT_TRUE(Open());
  EXPECT_TRUE(base::test::RunUntil([&] { return Field() == "banana"; }));
  EXPECT_TRUE(CountBecomes("1/1"));
}

IN_PROC_BROWSER_TEST_F(FindBarBrowserTest, CommandsGoToTheCompassBar) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL(kPage)));
  ASSERT_TRUE(Open());
  // Upstream's bar stays closed.
  EXPECT_FALSE(
      FindBarController::From(browser())->find_bar()->IsFindBarVisible());
  // Also on a tab switch, where it looks for tabs with an open bar.
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL(kPage), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  browser()->tab_strip_model()->ActivateTabAt(0);
  EXPECT_TRUE(base::test::RunUntil([&] { return bar()->IsShowing(); }));
  EXPECT_FALSE(
      FindBarController::From(browser())->find_bar()->IsFindBarVisible());
}

}  // namespace
}  // namespace compass
