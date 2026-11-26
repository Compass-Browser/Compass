// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/space_editor_bubble/space_editor_bubble.h"

#include <string>

#include "base/strings/utf_string_conversions.h"
#include "base/test/run_until.h"
#include "chrome/browser/ui/browser.h"
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

using SpaceEditorBubbleBrowserTest = InProcessBrowserTest;

views::Widget* BubbleWidget() {
  content::WebContents* contents = GetSpaceEditorContentsForTesting();
  return contents ? views::Widget::GetTopLevelWidgetForNativeView(
                        contents->GetNativeView())
                  : nullptr;
}

constexpr char kRoot[] =
    "document.querySelector('compass-space-editor-app').shadowRoot";

// The editor opens in a bubble once it's drawn, in the share sheet's look,
// and Esc closes it. The other tests are the sidebar's
// (CompassSidebarBrowserTest.SpaceEditor*, ThemeEditorEditsTheSpace).
IN_PROC_BROWSER_TEST_F(SpaceEditorBubbleBrowserTest, ShowsTheSpace) {
  SpaceModel& space = WindowFeatures::From(browser())->space_model();
  const gfx::Rect window =
      BrowserView::GetBrowserViewForBrowser(browser())->GetBoundsInScreen();

  // From the switcher, at the bottom of the sidebar.
  const gfx::Rect anchor(window.x() + 20, window.bottom() - 40, 24, 24);
  ShowSpaceEditorBubble(browser(), space.id().AsLowercaseString(), anchor);
  ASSERT_TRUE(base::test::RunUntil([] {
    views::Widget* widget = BubbleWidget();
    return widget && widget->IsVisible();
  }));
  // Upwards from there.
  EXPECT_LT(BubbleWidget()->GetWindowBoundsInScreen().y(), anchor.y());

  content::WebContents* page = GetSpaceEditorContentsForTesting();
  EXPECT_EQ(content::EvalJs(
                page, std::string(kRoot) + ".querySelector('#name').value"),
            base::UTF16ToUTF8(space.name()));
  EXPECT_NE(content::EvalJs(page,
                            "document.documentElement.style"
                            ".getPropertyValue('--background')"),
            "");
  // The last space can't be deleted; without sync there's no Share.
  EXPECT_EQ(true,
            content::EvalJs(
                page, std::string(kRoot) + ".querySelector('#delete').disabled"));
  EXPECT_EQ(true, content::EvalJs(
                      page, std::string(kRoot) + ".querySelector('#share') === null"));

  ASSERT_TRUE(content::ExecJs(
      page, "document.dispatchEvent(new KeyboardEvent('keydown', "
            "{key: 'Escape'}));"));
  ASSERT_TRUE(
      base::test::RunUntil([] { return !GetSpaceEditorContentsForTesting(); }));
}

}  // namespace
}  // namespace compass
