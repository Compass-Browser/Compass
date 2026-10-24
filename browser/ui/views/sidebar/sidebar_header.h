// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HEADER_H_
#define STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HEADER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/command_observer.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_strip_top_container.h"
#include "ui/base/interaction/element_identifier.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "url/gurl.h"

class BackForwardButton;
class BrowserWindowInterface;
class ReloadButton;
class ToolbarView;

namespace actions {
class ActionItem;
}  // namespace actions

namespace tabs {
class VerticalTabStripStateController;
}  // namespace tabs

namespace views {
class ImageButton;
class LabelButton;
}  // namespace views

namespace strata {

// The vertical tab strip region's top: upstream's row (traffic lights,
// collapse button, combo button) plus back, forward and reload at the row's
// right end and, under it, a pill with the active tab's host that opens the
// command bar, with a site info button at its left. In vertical tab mode it
// also holds the toolbar's extensions, pinned actions and app menu, and the
// toolbar is hidden. Collapsed, only the app menu shows. See
// docs/design/command-bar.md, "Slice 2" and "Slice 3".
class SidebarHeader : public VerticalTabStripTopContainer,
                      public CommandObserver,
                      public TabStripModelObserver {
  METADATA_HEADER(SidebarHeader, VerticalTabStripTopContainer)

 public:
  DECLARE_USER_DATA(SidebarHeader);
  DECLARE_CLASS_ELEMENT_IDENTIFIER_VALUE(kSiteInfoButtonElementId);

  SidebarHeader(tabs::VerticalTabStripStateController* state_controller,
                actions::ActionItem* root_action_item,
                BrowserWindowInterface* browser);
  SidebarHeader(const SidebarHeader&) = delete;
  SidebarHeader& operator=(const SidebarHeader&) = delete;
  ~SidebarHeader() override;

  static SidebarHeader* From(BrowserWindowInterface* browser);

  // The text the pill shows for |url|.
  static std::u16string GetPillText(const GURL& url);

  // The window's toolbar is initialized (|toolbar|) or about to be destroyed
  // (null). Takes the toolbar's controls in vertical tab mode; gives them
  // back before the toolbar goes away.
  void SetToolbar(ToolbarView* toolbar);

  // Whether the toolbar's controls are here, so the toolbar is hidden.
  bool HoldsToolbarControls() const { return !moved_views_.empty(); }

  BackForwardButton* back_button_for_testing() { return back_; }
  BackForwardButton* forward_button_for_testing() { return forward_; }
  ReloadButton* reload_button_for_testing() { return reload_; }
  views::LabelButton* pill_for_testing() { return pill_; }
  views::ImageButton* site_info_button_for_testing() { return site_info_; }

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
  // A toolbar view held here, and where it was in the toolbar.
  struct MovedView {
    raw_ptr<views::View> view;
    size_t toolbar_index;
  };

  // Moves the toolbar's extensions, pinned actions and app menu into their
  // slots, or back to the toolbar.
  void TakeToolbarControls();
  void ReturnToolbarControls();
  void OnModeChanged(tabs::VerticalTabStripStateController* controller);

  // Shows the active tab's URL in the pill, its security icon on the site
  // info button and its loading state on reload.
  void UpdateForActiveTab();
  void OnPillPressed();
  void OnSiteInfoPressed();

  const raw_ptr<tabs::VerticalTabStripStateController> state_controller_;
  const raw_ptr<BrowserWindowInterface> browser_;
  // Upstream's, in the top row.
  const raw_ptr<views::View> collapse_button_;
  const raw_ptr<views::View> combo_button_;
  raw_ptr<BackForwardButton> back_ = nullptr;
  raw_ptr<BackForwardButton> forward_ = nullptr;
  raw_ptr<ReloadButton> reload_ = nullptr;
  raw_ptr<views::LabelButton> pill_ = nullptr;
  raw_ptr<views::ImageButton> site_info_ = nullptr;
  // Whether the active tab has page info to show.
  bool show_site_info_ = false;
  // Hold the toolbar's views while they are here. The layout shows and hides
  // the slots, so upstream keeps control of the views' own visibility.
  raw_ptr<views::View> extensions_slot_ = nullptr;
  raw_ptr<views::View> pinned_actions_slot_ = nullptr;
  raw_ptr<views::View> app_menu_slot_ = nullptr;

  raw_ptr<ToolbarView> toolbar_ = nullptr;
  std::vector<MovedView> moved_views_;
  base::CallbackListSubscription mode_subscription_;
  ui::ScopedUnownedUserData<SidebarHeader> scoped_data_holder_;
};

// Creates the region's top container: a SidebarHeader. Called from
// VerticalTabStripRegionView (see patches/).
std::unique_ptr<VerticalTabStripTopContainer> CreateSidebarHeader(
    tabs::VerticalTabStripStateController* state_controller,
    actions::ActionItem* root_action_item,
    BrowserWindowInterface* browser);

// Hooks for BrowserView (see patches/): whether the toolbar is hidden because
// the sidebar holds its controls; the toolbar has been initialized; the
// toolbar is about to be destroyed.
bool ShouldHideToolbar(BrowserWindowInterface* browser);
void AttachToolbar(BrowserWindowInterface* browser, ToolbarView* toolbar);
void DetachToolbar(BrowserWindowInterface* browser);

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HEADER_H_
