// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_SIDEBAR_STRATA_SIDEBAR_VIEW_H_
#define STRATA_BROWSER_UI_VIEWS_SIDEBAR_STRATA_SIDEBAR_VIEW_H_

#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/webview/webview.h"

class BrowserWindowInterface;

namespace views {
class View;
}  // namespace views

namespace strata {

// Hosts chrome://strata-sidebar inside the vertical tab strip region.
class StrataSidebarView : public views::WebView {
  METADATA_HEADER(StrataSidebarView, views::WebView)

 public:
  explicit StrataSidebarView(BrowserWindowInterface* browser);
  StrataSidebarView(const StrataSidebarView&) = delete;
  StrataSidebarView& operator=(const StrataSidebarView&) = delete;
  ~StrataSidebarView() override;
};

// Puts the sidebar where |native_tab_strip| is in |content_parent| and hides
// the native strip, which stays in place because the region's drag, hover
// card and resize code rely on it. |bottom_container| stops growing so the
// sidebar gets the free space. Called from
// VerticalTabStripRegionView::AddTabStripView() (see patches/); safe to call
// again when the region re-adds its tab strip.
void InstallSidebar(views::View* content_parent,
                    views::View* native_tab_strip,
                    views::View* bottom_container,
                    BrowserWindowInterface* browser);

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_SIDEBAR_STRATA_SIDEBAR_VIEW_H_
