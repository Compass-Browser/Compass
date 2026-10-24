// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HEADER_H_
#define STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HEADER_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/command_observer.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_strip_top_container.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "url/gurl.h"

class BackForwardButton;
class BrowserWindowInterface;
class ReloadButton;

namespace actions {
class ActionItem;
}  // namespace actions

namespace tabs {
class VerticalTabStripStateController;
}  // namespace tabs

namespace views {
class LabelButton;
}  // namespace views

namespace strata {

// The vertical tab strip region's top: upstream's row (traffic lights,
// collapse button, combo button) plus back, forward and reload at the row's
// right end and, under it, a pill with the active tab's host that opens the
// command bar. Hidden parts when the region is collapsed. See
// docs/design/command-bar.md, "Slice 2".
class SidebarHeader : public VerticalTabStripTopContainer,
                      public CommandObserver,
                      public TabStripModelObserver {
  METADATA_HEADER(SidebarHeader, VerticalTabStripTopContainer)

 public:
  SidebarHeader(tabs::VerticalTabStripStateController* state_controller,
                actions::ActionItem* root_action_item,
                BrowserWindowInterface* browser);
  SidebarHeader(const SidebarHeader&) = delete;
  SidebarHeader& operator=(const SidebarHeader&) = delete;
  ~SidebarHeader() override;

  // The text the pill shows for |url|.
  static std::u16string GetPillText(const GURL& url);

  BackForwardButton* back_button_for_testing() { return back_; }
  BackForwardButton* forward_button_for_testing() { return forward_; }
  ReloadButton* reload_button_for_testing() { return reload_; }
  views::LabelButton* pill_for_testing() { return pill_; }

  // VerticalTabStripTopContainer:
  views::ProposedLayout CalculateProposedLayout(
      const views::SizeBounds& size_bounds) const override;
  bool IsPositionInWindowCaption(const gfx::Point& point) override;

  // CommandObserver:
  void EnabledStateChangedForCommand(int id, bool enabled) override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab,
                      TabChangeType change_type) override;

 private:
  // Shows the active tab's URL in the pill and its loading state on reload.
  void UpdateForActiveTab();
  void OnPillPressed();

  const raw_ptr<tabs::VerticalTabStripStateController> state_controller_;
  const raw_ptr<BrowserWindowInterface> browser_;
  // Upstream's, in the top row.
  const raw_ptr<views::View> collapse_button_;
  const raw_ptr<views::View> combo_button_;
  raw_ptr<BackForwardButton> back_ = nullptr;
  raw_ptr<BackForwardButton> forward_ = nullptr;
  raw_ptr<ReloadButton> reload_ = nullptr;
  raw_ptr<views::LabelButton> pill_ = nullptr;
};

// Creates the region's top container: a SidebarHeader. Called from
// VerticalTabStripRegionView (see patches/).
std::unique_ptr<VerticalTabStripTopContainer> CreateSidebarHeader(
    tabs::VerticalTabStripStateController* state_controller,
    actions::ActionItem* root_action_item,
    BrowserWindowInterface* browser);

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HEADER_H_
