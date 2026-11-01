// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/command_bar/command_bar.h"

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
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/web_contents_delegate.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/empty_tab.h"
#include "compass/browser/window_sync/window_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

// The field's text.
constexpr char kFieldText[] = R"(
  document.querySelector('compass-command-bar-app').shadowRoot
      .querySelector('#input').value)";

// Types $1 into the field, waits until the first result is for it, and
// presses Enter.
constexpr char kTypeAndEnter[] = R"(
  (async (text) => {
    const root = document.querySelector('compass-command-bar-app').shadowRoot;
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

IN_PROC_BROWSER_TEST_F(CommandBarBrowserTest, EscapeCloses) {
  content::WebContents* contents = OpenWith(IDC_NEW_TAB);
  ASSERT_TRUE(contents);

  // As the renderer host does with a key press: the bubble takes Esc before
  // the page sees it.
  input::NativeWebKeyboardEvent escape(
      blink::WebInputEvent::Type::kRawKeyDown,
      blink::WebInputEvent::kNoModifiers, base::TimeTicks::Now());
  escape.windows_key_code = ui::VKEY_ESCAPE;
  EXPECT_EQ(contents->GetDelegate()->PreHandleKeyboardEvent(contents, escape),
            content::KeyboardEventProcessingResult::HANDLED);

  EXPECT_TRUE(base::test::RunUntil([&] { return !bar()->IsShowing(); }));
  // Nothing was opened.
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
}

// Whether |window|'s Today section has an entry for |url|.
bool HasTodayEntry(BrowserWindowInterface* window, const GURL& url) {
  const SidebarNode* today =
      WindowFeatures::From(window)->space_model().today_root();
  for (const auto& entry : today->children()) {
    if (entry->url() == url) {
      return true;
    }
  }
  return false;
}

// Whether |window| shows only an empty tab, with its command bar open.
bool ShowsEmptyTab(BrowserWindowInterface* window) {
  TabStripModel* model = window->GetTabStripModel();
  return model->count() == 1 && IsEmptyTab(model->GetActiveTab()) &&
         CommandBar::From(window)->IsShowing();
}

// No new tab page (docs/design/command-bar.md, "No new tab page"): a new tab
// is the empty page, with the command bar over it, and no entry.
IN_PROC_BROWSER_TEST_F(CommandBarBrowserTest, NewTabPageIsTheEmptyPage) {
  const GURL url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  chrome::NewTab(browser(), NewTabTypes::kNewTabCommand);
  TabStripModel* model = browser()->tab_strip_model();
  ASSERT_EQ(model->count(), 2);
  content::WebContents* empty = model->GetActiveWebContents();
  ASSERT_TRUE(content::WaitForLoadStop(empty));
  // The empty page's own title: no new tab page loaded.
  EXPECT_EQ(empty->GetTitle(), u"Compass");
  EXPECT_TRUE(IsEmptyTab(empty));
  EXPECT_TRUE(base::test::RunUntil([&] { return bar()->IsShowing(); }));
  EXPECT_EQ(
      WindowFeatures::From(browser())->tab_strip_bridge().GetNode(
          model->GetActiveTab()),
      nullptr);

  // Going back to the page closes it.
  bar()->Close();
  model->ActivateTabAt(0);
  EXPECT_TRUE(base::test::RunUntil([&] { return model->count() == 1; }));
  EXPECT_EQ(model->GetActiveWebContents()->GetVisibleURL(), url);
}

// Closing the last tab leaves the window open on the empty page; what the
// command bar opens then takes its place.
IN_PROC_BROWSER_TEST_F(CommandBarBrowserTest, ClosingTheLastTabKeepsTheWindow) {
  const GURL first = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), first));
  chrome::CloseTab(browser());
  ASSERT_TRUE(base::test::RunUntil([&] { return ShowsEmptyTab(browser()); }));
  EXPECT_FALSE(browser()->IsDeleteScheduled());
  EXPECT_FALSE(HasTodayEntry(browser(), first));

  const GURL second = embedded_test_server()->GetURL("/title2.html");
  ASSERT_TRUE(content::ExecJs(bar()->web_contents_for_testing(),
                              content::JsReplace(kTypeAndEnter, second.spec())));
  TabStripModel* model = browser()->tab_strip_model();
  EXPECT_TRUE(base::test::RunUntil([&] {
    return model->count() == 1 &&
           model->GetActiveWebContents()->GetVisibleURL() == second;
  }));
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return HasTodayEntry(browser(), second); }));
}

// Closing the empty tab closes the window, as closing the window does.
IN_PROC_BROWSER_TEST_F(CommandBarBrowserTest, ClosingTheEmptyTabClosesTheWindow) {
  BrowserWindowInterface* second = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      second, embedded_test_server()->GetURL("/title1.html")));
  chrome::CloseTab(second);
  ASSERT_TRUE(base::test::RunUntil([&] { return ShowsEmptyTab(second); }));

  chrome::CloseTab(second);
  ui_test_utils::WaitForBrowserToClose(second);
}

// Sends a pointermove at screen position ($2, $3), moved by $4 pixels, to
// the $1th row, and returns the selected row's index.
constexpr char kPointerMove[] = R"(
  (async (index, x, y, movement) => {
    const root = document.querySelector('compass-command-bar-app').shadowRoot;
    while (root.querySelectorAll('.result').length <= index) {
      await new Promise(r => setTimeout(r, 20));
    }
    root.querySelectorAll('.result')[index].dispatchEvent(
        new PointerEvent('pointermove', {
          screenX: x, screenY: y, movementX: movement,
          bubbles: true, composed: true,
        }));
    await new Promise(r => setTimeout(r, 0));
    return Number(root.querySelector('.result[selected]').dataset.index);
  })($1, $2, $3, $4))";

IN_PROC_BROWSER_TEST_F(CommandBarBrowserTest, HoverNeedsRealMovement) {
  content::WebContents* contents = OpenWith(IDC_NEW_TAB);
  ASSERT_TRUE(contents);

  // Rows appearing under a resting pointer: no movement, no selection.
  EXPECT_EQ(0, content::EvalJs(contents, content::JsReplace(
                                             kPointerMove, 2, 100, 100, 0)));
  EXPECT_EQ(0, content::EvalJs(contents, content::JsReplace(
                                             kPointerMove, 2, 120, 100, 0)));
  // The pointer moves.
  EXPECT_EQ(2, content::EvalJs(contents, content::JsReplace(
                                             kPointerMove, 2, 140, 100, 20)));
}

}  // namespace
}  // namespace compass
