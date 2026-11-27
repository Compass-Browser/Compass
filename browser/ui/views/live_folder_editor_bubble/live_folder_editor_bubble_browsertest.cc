// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/live_folder_editor_bubble/live_folder_editor_bubble.h"

#include <optional>
#include <string>

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

using LiveFolderEditorBubbleBrowserTest = InProcessBrowserTest;

views::Widget* BubbleWidget() {
  content::WebContents* contents = GetLiveFolderEditorContentsForTesting();
  return contents ? views::Widget::GetTopLevelWidgetForNativeView(
                        contents->GetNativeView())
                  : nullptr;
}

constexpr char kRoot[] =
    "document.querySelector('compass-live-folder-editor-app').shadowRoot";

// New Live Folder… opens the plugin picker in a bubble once it's drawn, in
// the space editor's look; picking a plugin shows its form, and Esc backs
// out of it, then closes the bubble.
IN_PROC_BROWSER_TEST_F(LiveFolderEditorBubbleBrowserTest, NewFolder) {
  SpaceModel& space = WindowFeatures::From(browser())->space_model();
  const gfx::Rect window =
      BrowserView::GetBrowserViewForBrowser(browser())->GetBoundsInScreen();
  const gfx::Rect anchor(window.x() + 20, window.y() + 80, 24, 24);
  ShowLiveFolderEditorBubble(
      browser(), space.pinned_root()->id().AsLowercaseString(), std::nullopt,
      anchor);
  ASSERT_TRUE(base::test::RunUntil([] {
    views::Widget* widget = BubbleWidget();
    return widget && widget->IsVisible();
  }));
  // Downwards from a row in the window's upper half.
  EXPECT_GE(BubbleWidget()->GetWindowBoundsInScreen().y(), anchor.y());

  content::WebContents* page = GetLiveFolderEditorContentsForTesting();
  EXPECT_NE(content::EvalJs(page,
                            "document.documentElement.style"
                            ".getPropertyValue('--background')"),
            "");
  // The built-in plugins.
  EXPECT_LT(0, content::EvalJs(
                   page, std::string(kRoot) +
                             ".querySelectorAll('.live-editor-plugin').length")
                   .ExtractInt());

  ASSERT_TRUE(content::ExecJs(
      page, std::string(kRoot) + ".querySelector('.live-editor-plugin')"
                                 ".click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(page, std::string(kRoot) +
                                     ".querySelector('#form') !== null")
               .ExtractBool();
  }));

  const std::string escape =
      "document.dispatchEvent(new KeyboardEvent('keydown', "
      "{key: 'Escape', cancelable: true}));";
  ASSERT_TRUE(content::ExecJs(page, escape));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(page, std::string(kRoot) +
                                     ".querySelector('#plugins') !== null")
               .ExtractBool();
  }));
  ASSERT_TRUE(content::ExecJs(page, escape));
  ASSERT_TRUE(base::test::RunUntil(
      [] { return !GetLiveFolderEditorContentsForTesting(); }));
}

}  // namespace
}  // namespace compass
