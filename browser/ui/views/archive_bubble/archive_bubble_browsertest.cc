// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/archive_bubble/archive_bubble.h"

#include <string>

#include "base/test/run_until.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/widget/widget.h"

namespace compass {
namespace {

using ArchiveBubbleBrowserTest = InProcessBrowserTest;

views::Widget* BubbleWidget() {
  content::WebContents* contents = GetArchiveContentsForTesting();
  return contents ? views::Widget::GetTopLevelWidgetForNativeView(
                        contents->GetNativeView())
                  : nullptr;
}

constexpr char kRoot[] =
    "document.querySelector('compass-archive-app').shadowRoot";

// An empty archive says when entries are archived, in the space editor's
// look. The button toggles the bubble; Esc closes it too. Restoring is the
// sidebar's test (CompassSidebarBrowserTest.ArchivePanel).
IN_PROC_BROWSER_TEST_F(ArchiveBubbleBrowserTest, EmptyAndToggles) {
  const gfx::Rect window =
      BrowserView::GetBrowserViewForBrowser(browser())->GetBoundsInScreen();
  const gfx::Rect anchor(window.x() + 200, window.y() + 120, 20, 20);
  ToggleArchiveBubble(browser(), anchor);
  ASSERT_TRUE(base::test::RunUntil([] {
    views::Widget* widget = BubbleWidget();
    return widget && widget->IsVisible();
  }));

  content::WebContents* page = GetArchiveContentsForTesting();
  EXPECT_NE(content::EvalJs(page,
                            "document.documentElement.style"
                            ".getPropertyValue('--background')"),
            "");
  EXPECT_EQ(true, content::EvalJs(
                      page, std::string(kRoot) + ".querySelector('#hint') !== null"));
  EXPECT_EQ(true, content::EvalJs(
                      page, std::string(kRoot) + ".querySelector('#clear') === null"));

  // The button again closes it.
  ToggleArchiveBubble(browser(), anchor);
  ASSERT_TRUE(
      base::test::RunUntil([] { return !GetArchiveContentsForTesting(); }));

  // Right after, the click that closed it doesn't open it again.
  ToggleArchiveBubble(browser(), anchor);
  EXPECT_FALSE(GetArchiveContentsForTesting());
}

}  // namespace
}  // namespace compass
