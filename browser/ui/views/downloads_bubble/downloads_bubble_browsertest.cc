// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/run_until.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/views/download/bubble/download_toolbar_ui_controller.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/toolbar_button_provider.h"
#include "chrome/browser/ui/views/toolbar/pinned_toolbar_actions.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "compass/browser/downloads_folder/folder_files.h"
#include "compass/browser/ui/views/downloads_bubble/downloads_bubble_hooks.h"
#include "compass/browser/ui/views/downloads_bubble/downloads_library_view.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/layout/flex_layout_view.h"
#include "ui/views/widget/widget.h"

namespace compass::downloads_bubble {
namespace {

class DownloadsBubbleBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(downloads_.CreateUniqueTempDir());
    ASSERT_TRUE(screenshots_.CreateUniqueTempDir());
    DownloadPrefs::FromBrowserContext(browser()->GetProfile())
        ->SetDownloadPath(downloads_.GetPath());
    downloads_folder::SetScreenshotFolderForTesting(screenshots_.GetPath());
  }

  void TearDownOnMainThread() override {
    downloads_folder::SetScreenshotFolderForTesting(base::FilePath());
    InProcessBrowserTest::TearDownOnMainThread();
  }

  void Write(const base::ScopedTempDir& dir, const char* name) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    const base::FilePath path =
        dir.GetPath().Append(base::FilePath::FromUTF8Unsafe(name));
    ASSERT_TRUE(base::WriteFile(path, "x"));
    downloads_folder::MarkAsScreenCapture(path);
  }

  BrowserView* browser_view() {
    return BrowserView::GetBrowserViewForBrowser(browser());
  }

  base::ScopedTempDir downloads_;
  base::ScopedTempDir screenshots_;
};

// Pinned by default, so it's there before anything is downloaded.
IN_PROC_BROWSER_TEST_F(DownloadsBubbleBrowserTest, ButtonIsPinned) {
  PinnedToolbarActions* actions =
      browser_view()->toolbar_button_provider()->GetPinnedToolbarActions();
  ASSERT_TRUE(actions);
  EXPECT_TRUE(actions->IsActionPinned(kActionShowDownloads));
}

// With no recent downloads, the button opens the tabs on the folders.
IN_PROC_BROWSER_TEST_F(DownloadsBubbleBrowserTest,
                       ListsTheFoldersWithoutRecentDownloads) {
  Write(downloads_, "airdrop.pdf");
  Write(downloads_, "notes.txt");
  Write(screenshots_, "Screenshot 1.png");
  Write(screenshots_, "Screenshot 2.png");

  DownloadToolbarUIController::From(browser())->InvokeUI();
  DownloadsLibraryView* library = nullptr;
  ASSERT_TRUE(base::test::RunUntil([&] {
    library = GetBubbleLibraryForTesting(browser_view());
    return library && library->folder_row_count_for_testing() == 2 &&
           library->screenshot_count_for_testing() == 2;
  }));
  EXPECT_EQ(DownloadsLibraryView::Tab::kDownloads, library->selected_tab());
  library->SelectTab(DownloadsLibraryView::Tab::kScreenshots);
  EXPECT_EQ(DownloadsLibraryView::Tab::kScreenshots, library->selected_tab());

  // It follows the folder while it's open.
  Write(downloads_, "new.zip");
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return library->folder_row_count_for_testing() == 3; }));

  // Pressing the button again closes it.
  DownloadToolbarUIController::From(browser())->InvokeUI();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !GetBubbleLibraryForTesting(browser_view());
  }));
}

// Upstream's full bubble gets the tabs: its title gives way to them, and its
// rows and footer move into the Downloads tab.
IN_PROC_BROWSER_TEST_F(DownloadsBubbleBrowserTest, ExtendsUpstreamsDialog) {
  Write(downloads_, "file.txt");
  views::Widget::InitParams params(
      views::Widget::InitParams::CLIENT_OWNS_WIDGET,
      views::Widget::InitParams::TYPE_WINDOW_FRAMELESS);
  views::Widget widget;
  widget.Init(std::move(params));
  auto* dialog =
      widget.SetContentsView(std::make_unique<views::FlexLayoutView>());
  dialog->SetOrientation(views::LayoutOrientation::kVertical);
  auto* header = dialog->AddChildView(std::make_unique<views::FlexLayoutView>());
  auto* title = header->AddChildView(std::make_unique<views::Label>(u"Title"));
  auto* rows = dialog->AddChildView(std::make_unique<views::ScrollView>());
  rows->SetContents(std::make_unique<views::View>());
  dialog->AddChildView(std::make_unique<views::View>());

  ExtendDownloadDialog(dialog, rows, browser());

  ASSERT_EQ(2u, dialog->children().size());
  EXPECT_FALSE(title->GetVisible());
  EXPECT_EQ(2u, header->children().size());
  auto* library = views::AsViewClass<DownloadsLibraryView>(dialog->children()[1]);
  ASSERT_TRUE(library);
  EXPECT_TRUE(library->Contains(rows));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return library->folder_row_count_for_testing() == 1; }));
  EXPECT_LT(DialogWidth(), library->GetPreferredSize().width());
}

}  // namespace
}  // namespace compass::downloads_bubble
