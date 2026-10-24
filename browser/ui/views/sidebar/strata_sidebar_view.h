// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_SIDEBAR_STRATA_SIDEBAR_VIEW_H_
#define STRATA_BROWSER_UI_VIEWS_SIDEBAR_STRATA_SIDEBAR_VIEW_H_

#include <memory>

#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/webview/webview.h"

class BrowserWindowInterface;

namespace views {
class View;
}  // namespace views

namespace strata {

// Hosts chrome://strata-sidebar inside the vertical tab strip region, where
// the native tab strip would be.
class StrataSidebarView : public views::WebView {
  METADATA_HEADER(StrataSidebarView, views::WebView)

 public:
  explicit StrataSidebarView(BrowserWindowInterface* browser);
  StrataSidebarView(const StrataSidebarView&) = delete;
  StrataSidebarView& operator=(const StrataSidebarView&) = delete;
  ~StrataSidebarView() override;
};

// The sidebar the vertical tab strip region shows in place of the native tab
// strip, which Strata never builds. Called from
// VerticalTabStripRegionView::InitializeTabStrip() (see patches/), which adds
// it where the native strip would go.
std::unique_ptr<views::View> CreateSidebar(BrowserWindowInterface* browser);

// Lays out |sidebar| once the region has added it: it takes the free space,
// so |bottom_container| stops growing.
void InstallSidebar(views::View* sidebar, views::View* bottom_container);

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_SIDEBAR_STRATA_SIDEBAR_VIEW_H_
