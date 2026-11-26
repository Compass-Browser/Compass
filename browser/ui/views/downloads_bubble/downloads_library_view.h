// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_DOWNLOADS_BUBBLE_DOWNLOADS_LIBRARY_VIEW_H_
#define COMPASS_BROWSER_UI_VIEWS_DOWNLOADS_BUBBLE_DOWNLOADS_LIBRARY_VIEW_H_

#include <memory>
#include <vector>

#include "base/callback_list.h"
#include "base/containers/flat_set.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "base/timer/timer.h"
#include "compass/browser/downloads_folder/folder_files.h"
#include "compass/browser/ui/views/downloads_bubble/file_thumbnails.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

class BrowserView;
class Profile;

namespace views {
class Label;
class LabelButton;
class ScrollView;
}  // namespace views

namespace compass::downloads_bubble {

class FolderWatch;

// The downloads bubble's body (docs/design/downloads-bubble.md): two tabs,
// Downloads and Screenshots, each a list that follows its folder while the
// bubble is open.
//
// Downloads: upstream's rows for the browser's recent downloads, if any,
// then the download folder's other files, newest first. Screenshots: a grid
// of the screenshot folder's screenshots and screen recordings.
class DownloadsLibraryView : public views::View {
  METADATA_HEADER(DownloadsLibraryView, views::View)

 public:
  enum class Tab { kDownloads, kScreenshots };

  // The files upstream's rows show, so the folder's list leaves them out.
  using RowPathsCallback =
      base::RepeatingCallback<base::flat_set<base::FilePath>()>;

  // |browser_rows|: upstream's rows (a scroll view sized to them), shown
  // first under Downloads. |downloads_footer|: what ends the Downloads tab
  // ("Show all downloads"). Either may be null. |tint|: the window's space's
  // color, which the bubble takes (none for a grey space or off the record).
  DownloadsLibraryView(Profile* profile,
                       std::optional<SkColor> tint,
                       std::unique_ptr<views::View> browser_rows,
                       RowPathsCallback row_paths,
                       std::unique_ptr<views::View> downloads_footer);
  DownloadsLibraryView(const DownloadsLibraryView&) = delete;
  DownloadsLibraryView& operator=(const DownloadsLibraryView&) = delete;
  ~DownloadsLibraryView() override;

  // The tabs, for the bubble's header, which owns them.
  std::unique_ptr<views::View> CreateTabStrip();

  void SelectTab(Tab tab);
  Tab selected_tab() const { return tab_; }

  // Lists both folders again (it also does when they change).
  void Refresh();

  // views::View:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  void OnThemeChanged() override;
  void AddedToWidget() override;

  size_t folder_row_count_for_testing() const;
  size_t screenshot_count_for_testing() const;
  // Runs after each time the lists are rebuilt.
  void set_on_refreshed_for_testing(base::RepeatingClosure callback) {
    on_refreshed_for_testing_ = std::move(callback);
  }

 private:
  struct Listing;

  void OnListed(std::unique_ptr<Listing> listing);
  void RebuildDownloads();
  void RebuildScreenshots();
  void RefreshSoon();
  void OpenScreenshotFolder();
  // Colors everything from |tint_| and the theme, the bubble's frame too.
  void ApplyLook();

  const raw_ptr<Profile> profile_;
  RowPathsCallback row_paths_;
  Tab tab_ = Tab::kDownloads;
  base::RepeatingCallbackList<void(Tab)> tab_callbacks_;

  const std::optional<SkColor> tint_;

  // Downloads: ["Recent downloads"][a card with upstream's rows]
  // ["In your Downloads folder"][a card with the folder's files][empty].
  raw_ptr<views::View> downloads_page_ = nullptr;
  raw_ptr<views::ScrollView> downloads_scroll_ = nullptr;
  raw_ptr<views::Label> recent_caption_ = nullptr;
  raw_ptr<views::View> recent_card_ = nullptr;
  raw_ptr<views::View> browser_rows_ = nullptr;
  raw_ptr<views::Label> folder_caption_ = nullptr;
  raw_ptr<views::View> folder_rows_ = nullptr;
  raw_ptr<views::Label> downloads_empty_ = nullptr;

  // Screenshots: [the folder, Open Folder][the grid][empty].
  raw_ptr<views::View> screenshots_page_ = nullptr;
  raw_ptr<views::ScrollView> screenshots_scroll_ = nullptr;
  raw_ptr<views::Label> screenshots_caption_ = nullptr;
  raw_ptr<views::LabelButton> open_folder_ = nullptr;
  raw_ptr<views::View> grid_ = nullptr;
  raw_ptr<views::Label> screenshots_empty_ = nullptr;

  std::vector<downloads_folder::FolderFile> downloads_files_;
  std::vector<downloads_folder::FolderFile> screenshot_files_;
  base::FilePath download_folder_;
  base::FilePath screenshot_folder_;
  bool same_folder_ = false;

  FileThumbnails thumbnails_;
  base::SequenceBound<FolderWatch> watch_;
  base::OneShotTimer refresh_timer_;
  bool listing_ = false;
  bool list_again_ = false;
  base::RepeatingClosure on_refreshed_for_testing_;
  base::WeakPtrFactory<DownloadsLibraryView> weak_factory_{this};
};

// The library in the bubble ShowWithoutRecentDownloads() opened, while it's
// open.
DownloadsLibraryView* GetBubbleLibraryForTesting(BrowserView* browser_view);

}  // namespace compass::downloads_bubble

#endif  // COMPASS_BROWSER_UI_VIEWS_DOWNLOADS_BUBBLE_DOWNLOADS_LIBRARY_VIEW_H_
