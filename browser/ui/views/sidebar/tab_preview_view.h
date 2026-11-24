// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_SIDEBAR_TAB_PREVIEW_VIEW_H_
#define COMPASS_BROWSER_UI_VIEWS_SIDEBAR_TAB_PREVIEW_VIEW_H_

#include "base/functional/callback.h"
#include "base/time/time.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

namespace compass {

struct LivePreviewItem;
struct TabPreview;

// A plugin's tab preview in a sidebar hover card, where the screenshot would
// go (docs/design/live-folders.md, "Tab previews"): an optional header, a
// row per item (title, subtitle, a badge, how long ago) and an optional
// footer. Rows light up under the pointer and take clicks; the card can't
// take focus, so not the keyboard.
class TabPreviewView : public views::View {
  METADATA_HEADER(TabPreviewView, views::View)

 public:
  // A row was clicked: its item, and the click's ui::EventFlags (for
  // ui::DispositionFromEventFlags()).
  using ItemCallback =
      base::RepeatingCallback<void(const LivePreviewItem& item, int flags)>;

  // |now| for "2 hr ago". Without |on_click|, rows are display only.
  TabPreviewView(const TabPreview& preview,
                 base::Time now,
                 ItemCallback on_click = ItemCallback());
  // A slim "Loading…" row while the plugin runs.
  TabPreviewView();
  TabPreviewView(const TabPreviewView&) = delete;
  TabPreviewView& operator=(const TabPreviewView&) = delete;
  ~TabPreviewView() override;

  bool is_loading() const { return loading_; }
  // Whether it has rows that take clicks.
  bool has_clickable_rows() const { return has_clickable_rows_; }

 private:
  bool loading_ = false;
  bool has_clickable_rows_ = false;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_SIDEBAR_TAB_PREVIEW_VIEW_H_
