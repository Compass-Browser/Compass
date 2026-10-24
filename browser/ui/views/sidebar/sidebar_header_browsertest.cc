// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/sidebar_header.h"

#include <string>

#include "base/test/run_until.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/shared/tab_strip_combo_button.h"
#include "chrome/browser/ui/views/toolbar/back_forward_button.h"
#include "chrome/browser/ui/views/toolbar/reload_button.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "strata/browser/ui/views/command_bar/command_bar.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/event.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/test/button_test_api.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"
#include "url/gurl.h"

namespace strata {
namespace {

template <typename T>
T* FindDescendant(views::View* root) {
  if (auto* match = views::AsViewClass<T>(root)) {
    return match;
  }
  for (views::View* child : root->children()) {
    if (T* match = FindDescendant<T>(child)) {
      return match;
    }
  }
  return nullptr;
}

void Click(views::Button* button) {
  views::test::ButtonTestApi(button).NotifyClick(ui::MouseEvent(
      ui::EventType::kMousePressed, gfx::Point(), gfx::Point(),
      ui::EventTimeForNow(), ui::EF_LEFT_MOUSE_BUTTON, 0));
}

class SidebarHeaderBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
  }

  views::View* browser_view() {
    return BrowserView::GetBrowserViewForBrowser(browser());
  }
  SidebarHeader* header() {
    return FindDescendant<SidebarHeader>(browser_view());
  }
  std::u16string pill_text() {
    return std::u16string(header()->pill_for_testing()->GetText());
  }
};

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, ShowsNavButtonsAndPill) {
  ASSERT_TRUE(header());
  views::View* back = header()->back_button_for_testing();
  views::View* forward = header()->forward_button_for_testing();
  views::View* reload = header()->reload_button_for_testing();
  views::View* pill = header()->pill_for_testing();
  ASSERT_TRUE(base::test::RunUntil([&] { return !pill->bounds().IsEmpty(); }));

  for (views::View* view : {back, forward, reload, pill}) {
    EXPECT_TRUE(view->GetVisible());
    EXPECT_FALSE(view->bounds().IsEmpty());
  }
  // One row, in order, on the collapse button's line, and the pill under it.
  const int row_center =
      header()->GetCollapseButton()->bounds().CenterPoint().y();
  EXPECT_EQ(back->bounds().CenterPoint().y(), row_center);
  EXPECT_LT(back->bounds().right(), forward->bounds().x());
  EXPECT_LT(forward->bounds().right(), reload->bounds().x());
  EXPECT_GT(back->bounds().x(), header()->GetCollapseButton()->bounds().right());
  EXPECT_GE(pill->y(), reload->bounds().bottom());
  EXPECT_EQ(pill->width(), header()->width());
  // Upstream's combo button is unpinned by default.
  EXPECT_TRUE(header()->GetComboButton()->bounds().IsEmpty() ||
              !header()->GetComboButton()->GetVisible());
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, BackAndForward) {
  const GURL first = embedded_test_server()->GetURL("/title1.html");
  const GURL second = embedded_test_server()->GetURL("/title2.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), first));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), second));
  views::Button* back = header()->back_button_for_testing();
  views::Button* forward = header()->forward_button_for_testing();
  EXPECT_TRUE(back->GetEnabled());
  EXPECT_FALSE(forward->GetEnabled());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  content::TestNavigationObserver back_observer(contents);
  Click(back);
  back_observer.Wait();
  EXPECT_EQ(contents->GetLastCommittedURL(), first);
  EXPECT_TRUE(base::test::RunUntil([&] { return forward->GetEnabled(); }));

  content::TestNavigationObserver forward_observer(contents);
  Click(forward);
  forward_observer.Wait();
  EXPECT_EQ(contents->GetLastCommittedURL(), second);
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, PillFollowsTheActiveTab) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("www.a.test", "/title1.html")));
  EXPECT_EQ(pill_text(), u"a.test");

  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  EXPECT_EQ(pill_text(), u"Search or enter URL");

  browser()->tab_strip_model()->ActivateTabAt(0);
  EXPECT_EQ(pill_text(), u"a.test");
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, PillOpensTheCommandBar) {
  const GURL url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  CommandBar* bar = CommandBar::From(browser());
  ASSERT_TRUE(bar);

  Click(header()->pill_for_testing());

  ASSERT_TRUE(base::test::RunUntil([&] { return bar->IsShowing(); }));
  EXPECT_EQ(content::EvalJs(bar->web_contents_for_testing(), R"(
      document.querySelector('strata-command-bar-app').shadowRoot
          .querySelector('#input').value)"),
            url.spec());
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, CollapsingHidesTheHeader) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  views::View* pill = header()->pill_for_testing();
  ASSERT_TRUE(base::test::RunUntil([&] { return pill->GetVisible(); }));

  region->RequestCollapse(true);
  EXPECT_TRUE(base::test::RunUntil([&] {
    return !pill->GetVisible() &&
           !header()->back_button_for_testing()->GetVisible();
  }));

  region->RequestCollapse(false);
  EXPECT_TRUE(base::test::RunUntil([&] { return pill->GetVisible(); }));
}

}  // namespace
}  // namespace strata
