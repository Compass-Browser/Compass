// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/command_bar/command_bar.h"

#include <string>

#include "base/test/run_until.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/window_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace strata {
namespace {

// The field's text.
constexpr char kFieldText[] = R"(
  document.querySelector('strata-command-bar-app').shadowRoot
      .querySelector('#input').value)";

// Types $1 into the field, waits until the first result is for it, and
// presses Enter.
constexpr char kTypeAndEnter[] = R"(
  (async (text) => {
    const root = document.querySelector('strata-command-bar-app').shadowRoot;
    const input = root.querySelector('#input');
    input.value = text;
    input.dispatchEvent(new InputEvent('input', {inputType: 'insertText'}));
    const first = () => root.querySelector('.result .title')?.textContent;
    while (!first()?.includes(text)) {
      await new Promise(r => setTimeout(r, 20));
    }
    input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));
  })($1))";

class CommandBarBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(embedded_test_server()->Start());
  }

  CommandBar* bar() { return CommandBar::From(browser()); }

  // Runs |command| and waits for the bar to show.
  content::WebContents* OpenWith(int command) {
    chrome::ExecuteCommand(browser(), command);
    EXPECT_TRUE(base::test::RunUntil([&] { return bar()->IsShowing(); }));
    return bar()->web_contents_for_testing();
  }
};

IN_PROC_BROWSER_TEST_F(CommandBarBrowserTest, NewTabOpensANewEntry) {
  ASSERT_TRUE(bar());
  content::WebContents* contents = OpenWith(IDC_NEW_TAB);
  ASSERT_TRUE(contents);
  // No tab yet: the bar stands in for the new tab page.
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
  EXPECT_EQ(content::EvalJs(contents, kFieldText), "");

  const GURL url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(content::ExecJs(contents,
                              content::JsReplace(kTypeAndEnter, url.spec())));

  TabStripModel* model = browser()->tab_strip_model();
  ASSERT_TRUE(base::test::RunUntil([&] { return model->count() == 2; }));
  EXPECT_EQ(model->GetActiveWebContents()->GetVisibleURL(), url);
  EXPECT_TRUE(base::test::RunUntil([&] { return !bar()->IsShowing(); }));
  // It's a Today entry.
  const SidebarNode* today =
      WindowFeatures::From(browser())->space_model().today_root();
  EXPECT_TRUE(base::test::RunUntil([&] {
    for (const auto& entry : today->children()) {
      if (entry->url() == url) {
        return true;
      }
    }
    return false;
  }));
}

IN_PROC_BROWSER_TEST_F(CommandBarBrowserTest, FocusLocationLoadsInPlace) {
  const GURL first = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), first));

  content::WebContents* contents = OpenWith(IDC_FOCUS_LOCATION);
  ASSERT_TRUE(contents);
  // It starts from the page's URL.
  EXPECT_EQ(content::EvalJs(contents, kFieldText), first.spec());

  const GURL second = embedded_test_server()->GetURL("/title2.html");
  ASSERT_TRUE(content::ExecJs(
      contents, content::JsReplace(kTypeAndEnter, second.spec())));

  TabStripModel* model = browser()->tab_strip_model();
  EXPECT_TRUE(base::test::RunUntil([&] {
    return model->GetActiveWebContents()->GetVisibleURL() == second;
  }));
  EXPECT_EQ(model->count(), 1);
}

IN_PROC_BROWSER_TEST_F(CommandBarBrowserTest, OpeningAgainStartsOver) {
  const GURL url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  content::WebContents* contents = OpenWith(IDC_NEW_TAB);
  ASSERT_TRUE(contents);
  ASSERT_EQ(content::EvalJs(contents, kFieldText), "");

  // Cmd+L while it's open switches to the current entry.
  chrome::ExecuteCommand(browser(), IDC_FOCUS_LOCATION);
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return content::EvalJs(contents, kFieldText) == url.spec(); }));
  EXPECT_TRUE(bar()->IsShowing());

  // Closed, it opens again.
  bar()->Close();
  EXPECT_FALSE(bar()->IsShowing());
  EXPECT_EQ(OpenWith(IDC_NEW_TAB), contents);
  EXPECT_EQ(content::EvalJs(contents, kFieldText), "");
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
}

}  // namespace
}  // namespace strata
