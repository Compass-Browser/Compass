// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/share_bubble/share_bubble.h"

#include <string>

#include "base/strings/utf_string_conversions.h"
#include "base/test/run_until.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/window_features.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/widget/widget.h"

namespace compass {
namespace {

using ShareBubbleBrowserTest = InProcessBrowserTest;

// Every window, popups too.
int CountWindows() {
  int count = 0;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* window) {
        ++count;
        return true;
      });
  return count;
}

views::Widget* BubbleWidget() {
  content::WebContents* contents = GetShareBubbleContentsForTesting();
  return contents ? views::Widget::GetTopLevelWidgetForNativeView(
                        contents->GetNativeView())
                  : nullptr;
}

// The sheet opens in a bubble of the window, not a window of its own, and
// shows once it's drawn.
IN_PROC_BROWSER_TEST_F(ShareBubbleBrowserTest, ShowsTheSheetInABubble) {
  SpaceModel& space = WindowFeatures::From(browser())->space_model();
  const int windows = CountWindows();
  const gfx::Rect anchor =
      BrowserView::GetBrowserViewForBrowser(browser())->GetBoundsInScreen();

  ShowShareBubble(browser(), "space", space.id().AsLowercaseString(),
                  gfx::Rect(anchor.x() + 100, anchor.y() + 100, 0, 32));
  ASSERT_TRUE(base::test::RunUntil([] {
    views::Widget* widget = BubbleWidget();
    return widget && widget->IsVisible();
  }));
  EXPECT_EQ(CountWindows(), windows);

  content::WebContents* page = GetShareBubbleContentsForTesting();
  EXPECT_EQ(content::EvalJs(page, "document.querySelector('#title')"
                                  ".textContent"),
            "Share “" + base::UTF16ToUTF8(space.name()) + "”");
  // The downloads bubble's width.
  EXPECT_EQ(content::EvalJs(page, "document.body.offsetWidth"), 420);
  // The browser's colors, as the frame has them.
  EXPECT_NE(content::EvalJs(page,
                            "document.documentElement.style"
                            ".getPropertyValue('--background')"),
            "");

  // Close from the page.
  EXPECT_TRUE(content::ExecJs(page, "document.querySelector('#close')"
                                    ".click()"));
  ASSERT_TRUE(base::test::RunUntil(
      [] { return !GetShareBubbleContentsForTesting(); }));
}

// Opening another sheet replaces the open one.
IN_PROC_BROWSER_TEST_F(ShareBubbleBrowserTest, OneAtATime) {
  SpaceModel& space = WindowFeatures::From(browser())->space_model();
  const gfx::Rect anchor =
      BrowserView::GetBrowserViewForBrowser(browser())->GetBoundsInScreen();
  const std::string id = space.id().AsLowercaseString();

  ShowShareBubble(browser(), "space", id, anchor);
  views::Widget* first = BubbleWidget();
  ASSERT_TRUE(first);
  base::WeakPtr<views::Widget> first_widget = first->GetWeakPtr();
  ShowShareBubble(browser(), "space", id, anchor);
  // Closed at once.
  EXPECT_TRUE(!first_widget || first_widget->IsClosed());
  ASSERT_TRUE(GetShareBubbleContentsForTesting());
  ASSERT_TRUE(base::test::RunUntil([] {
    views::Widget* widget = BubbleWidget();
    return widget && widget->IsVisible();
  }));
}

}  // namespace
}  // namespace compass
