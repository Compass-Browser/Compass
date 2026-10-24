// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_SIDEBAR_STRATA_SIDEBAR_VIEW_H_
#define STRATA_BROWSER_UI_VIEWS_SIDEBAR_STRATA_SIDEBAR_VIEW_H_

#include <memory>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "components/tabs/public/tab_interface.h"
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

// Reports |browser|'s active tab, as the native strip's node tree did for the
// region: now, whenever it changes, and null as soon as the active tab leaves
// the window. A closing window closes its tabs without changing the active
// one, so without that a listener could keep using a destroyed tab.
class ActiveTabWatcher {
 public:
  using Callback = base::RepeatingCallback<void(const tabs::TabInterface*)>;

  ActiveTabWatcher(BrowserWindowInterface* browser, Callback callback);
  ActiveTabWatcher(const ActiveTabWatcher&) = delete;
  ActiveTabWatcher& operator=(const ActiveTabWatcher&) = delete;
  ~ActiveTabWatcher();

 private:
  void OnActiveTabChanged(BrowserWindowInterface* browser);
  void OnWillDetach(tabs::TabInterface* tab,
                    tabs::TabInterface::DetachReason reason);

  Callback callback_;
  base::CallbackListSubscription active_tab_subscription_;
  base::CallbackListSubscription will_detach_subscription_;
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_SIDEBAR_STRATA_SIDEBAR_VIEW_H_
