// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/peek/peek.h"

#include "base/test/run_until.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "compass/browser/window_sync/window_features.h"
#include "components/tabs/public/tab_interface.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/animation_test_api.h"
#include "ui/gfx/geometry/point.h"

namespace compass {
namespace {

// A page that is one big link to /title2.html.
constexpr char kLinkPage[] = R"(
  document.body.innerHTML =
      '<a id="link" href="/title2.html" ' +
      'style="display:block;width:200px;height:200px">Link</a>';
)";

class PeekBrowserTest : public InProcessBrowserTest {
 public:
  explicit PeekBrowserTest(gfx::Animation::RichAnimationRenderMode mode =
                               gfx::Animation::RichAnimationRenderMode::
                                   FORCE_DISABLED)
      : animation_mode_(
            gfx::AnimationTestApi::SetRichAnimationRenderMode(mode)) {}

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
  }

 protected:
  Peek* peek() { return Peek::From(browser()); }
  TabStripModel* tabs() { return browser()->tab_strip_model(); }
  content::WebContents* active_contents() {
    return tabs()->GetActiveWebContents();
  }

  // Option+clicks the link on a page of a.test and waits for Peek to show
  // /title2.html.
  void PeekAtLink() {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL("a.test", "/title1.html")));
    ASSERT_TRUE(content::ExecJs(active_contents(), kLinkPage));
    content::SimulateMouseClickAt(active_contents(),
                                  blink::WebInputEvent::kAltKey,
                                  blink::WebMouseEvent::Button::kLeft,
                                  gfx::Point(50, 50));
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return peek()->IsShowing() && peek()->web_contents(); }));
    ASSERT_TRUE(content::WaitForLoadStop(peek()->web_contents()));
    ASSERT_EQ(peek()->web_contents()->GetLastCommittedURL().path(),
              "/title2.html");
  }

 private:
  gfx::AnimationTestApi::RenderModeResetter animation_mode_;
};

IN_PROC_BROWSER_TEST_F(PeekBrowserTest, OptionClickPeeksInsteadOfDownloading) {
  ASSERT_NO_FATAL_FAILURE(PeekAtLink());
  // No tab, no entry, and the page underneath stays.
  EXPECT_EQ(tabs()->count(), 1);
  EXPECT_EQ(active_contents()->GetLastCommittedURL().path(), "/title1.html");
  EXPECT_NE(peek()->web_contents(), active_contents());
  // Over the content area.
  EXPECT_TRUE(peek()->widget_for_testing()->IsVisible());
}

IN_PROC_BROWSER_TEST_F(PeekBrowserTest, EscapeCloses) {
  ASSERT_NO_FATAL_FAILURE(PeekAtLink());
  // As the renderer host does with a key the page didn't handle (synthetic
  // key presses don't come back to the delegate in tests).
  content::WebContents* page = peek()->web_contents();
  input::NativeWebKeyboardEvent escape(blink::WebInputEvent::Type::kRawKeyDown,
                                       blink::WebInputEvent::kNoModifiers,
                                       base::TimeTicks::Now());
  escape.windows_key_code = ui::VKEY_ESCAPE;
  EXPECT_TRUE(page->GetDelegate()->HandleKeyboardEvent(page, escape));
  EXPECT_TRUE(base::test::RunUntil([&] { return !peek()->IsShowing(); }));
  EXPECT_FALSE(peek()->web_contents());
  EXPECT_EQ(tabs()->count(), 1);
}

// Cmd+W closes Peek, not the tab under it.
IN_PROC_BROWSER_TEST_F(PeekBrowserTest, CloseTabClosesPeek) {
  ASSERT_NO_FATAL_FAILURE(PeekAtLink());
  chrome::ExecuteCommand(browser(), IDC_CLOSE_TAB);
  EXPECT_FALSE(peek()->IsShowing());
  EXPECT_EQ(tabs()->count(), 1);
}

// Cmd+O keeps the page: the same WebContents becomes the active tab.
IN_PROC_BROWSER_TEST_F(PeekBrowserTest, OpenFileOpensItAsATab) {
  ASSERT_NO_FATAL_FAILURE(PeekAtLink());
  content::WebContents* page = peek()->web_contents();
  chrome::ExecuteCommand(browser(), IDC_OPEN_FILE);
  EXPECT_FALSE(peek()->IsShowing());
  EXPECT_EQ(tabs()->count(), 2);
  EXPECT_EQ(active_contents(), page);
  EXPECT_EQ(page->GetLastCommittedURL().path(), "/title2.html");
}

IN_PROC_BROWSER_TEST_F(PeekBrowserTest, SwitchingTabsCloses) {
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_NO_FATAL_FAILURE(PeekAtLink());
  tabs()->ActivateTabAt(tabs()->active_index() == 0 ? 1 : 0);
  EXPECT_TRUE(base::test::RunUntil([&] { return !peek()->IsShowing(); }));
}

// Links in the peeked page load in Peek; new tabs open in the window.
IN_PROC_BROWSER_TEST_F(PeekBrowserTest, LinksInThePage) {
  ASSERT_NO_FATAL_FAILURE(PeekAtLink());
  content::WebContents* page = peek()->web_contents();
  ASSERT_TRUE(content::ExecJs(page, "location.href = '/title3.html'"));
  ASSERT_TRUE(content::WaitForLoadStop(page));
  EXPECT_EQ(page->GetLastCommittedURL().path(), "/title3.html");
  EXPECT_EQ(tabs()->count(), 1);

  // A new tab shows over Peek, which closes.
  ui_test_utils::TabAddedWaiter tab_added(browser());
  content::ExecuteScriptAsync(page, "window.open('/title1.html')");
  tab_added.Wait();
  EXPECT_EQ(tabs()->count(), 2);
  EXPECT_TRUE(base::test::RunUntil([&] { return !peek()->IsShowing(); }));
}

// The page's title and host show above the card, and follow its navigations.
IN_PROC_BROWSER_TEST_F(PeekBrowserTest, TitleAndHostAboveTheCard) {
  ASSERT_NO_FATAL_FAILURE(PeekAtLink());
  EXPECT_TRUE(base::test::RunUntil([&] {
    return peek()->GetTitleForTesting() == u"Title Of Awesomeness";
  }));
  EXPECT_EQ(peek()->GetHostForTesting(), u"a.test");

  content::WebContents* page = peek()->web_contents();
  ASSERT_TRUE(content::ExecJs(page, "document.title = 'Renamed'"));
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return peek()->GetTitleForTesting() == u"Renamed"; }));
}

class PeekAnimatedBrowserTest : public PeekBrowserTest {
 public:
  PeekAnimatedBrowserTest()
      : PeekBrowserTest(
            gfx::Animation::RichAnimationRenderMode::FORCE_ENABLED) {}
};

// Closing fades the card out before it goes; Peek counts as closed at once,
// so Cmd+W closes the tab again.
IN_PROC_BROWSER_TEST_F(PeekAnimatedBrowserTest, ClosingFadesOut) {
  ASSERT_NO_FATAL_FAILURE(PeekAtLink());
  views::Widget* widget = peek()->widget_for_testing();
  peek()->Close();
  EXPECT_FALSE(peek()->IsShowing());
  // Still on screen while it fades.
  EXPECT_EQ(peek()->widget_for_testing(), widget);
  EXPECT_TRUE(peek()->web_contents());
  EXPECT_TRUE(base::test::RunUntil([&] { return !peek()->web_contents(); }));
  EXPECT_FALSE(peek()->widget_for_testing());
  EXPECT_EQ(tabs()->count(), 1);
}

// Another Option+click while the card fades out shows the new page.
IN_PROC_BROWSER_TEST_F(PeekAnimatedBrowserTest, PeekingAgainWhileClosing) {
  ASSERT_NO_FATAL_FAILURE(PeekAtLink());
  peek()->Close();
  content::SimulateMouseClickAt(active_contents(),
                                blink::WebInputEvent::kAltKey,
                                blink::WebMouseEvent::Button::kLeft,
                                gfx::Point(50, 50));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return peek()->IsShowing() && peek()->web_contents(); }));
  ASSERT_TRUE(content::WaitForLoadStop(peek()->web_contents()));
  EXPECT_EQ(peek()->web_contents()->GetLastCommittedURL().path(),
            "/title2.html");
}

// A target=_blank link from a pinned tab peeks instead of opening a tab;
// the same link from a Today entry opens a tab.
IN_PROC_BROWSER_TEST_F(PeekBrowserTest, PinnedTabLinksToNewTabsPeek) {
  constexpr char kBlankLink[] = R"(
    document.body.innerHTML =
        '<a id="link" href="/title2.html" target="_blank" ' +
        'style="display:block;width:200px;height:200px">Link</a>';
  )";
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("a.test", "/title1.html")));
  ASSERT_TRUE(content::ExecJs(active_contents(), kBlankLink));

  // From Today: a new tab.
  {
    ui_test_utils::TabAddedWaiter tab_added(browser());
    content::SimulateMouseClickAt(active_contents(), 0,
                                  blink::WebMouseEvent::Button::kLeft,
                                  gfx::Point(50, 50));
    tab_added.Wait();
  }
  EXPECT_FALSE(peek()->IsShowing());
  ASSERT_EQ(tabs()->count(), 2);
  tabs()->CloseWebContentsAt(tabs()->active_index(),
                             TabCloseTypes::CLOSE_NONE);
  ASSERT_EQ(tabs()->count(), 1);

  // Pinned: Peek.
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel& model = features->space_model();
  SidebarNode* node = features->tab_strip_bridge().GetNode(
      tabs::TabInterface::GetFromContents(active_contents()));
  ASSERT_TRUE(node);
  model.Move(node, model.pinned_root(), 0);
  content::WebContents* pinned = active_contents();

  content::SimulateMouseClickAt(pinned, 0, blink::WebMouseEvent::Button::kLeft,
                                gfx::Point(50, 50));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return peek()->IsShowing() && peek()->web_contents(); }));
  ASSERT_TRUE(content::WaitForLoadStop(peek()->web_contents()));
  EXPECT_EQ(peek()->web_contents()->GetLastCommittedURL().path(),
            "/title2.html");
  EXPECT_EQ(tabs()->count(), 1);
  EXPECT_EQ(active_contents(), pinned);

  // Kept, it becomes a Today entry.
  chrome::ExecuteCommand(browser(), IDC_OPEN_FILE);
  EXPECT_EQ(tabs()->count(), 2);
  EXPECT_EQ(active_contents()->GetLastCommittedURL().path(), "/title2.html");
}

}  // namespace
}  // namespace compass
