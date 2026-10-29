// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HEADER_H_
#define STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HEADER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/command_observer.h"
#include "chrome/browser/ui/content_settings/content_setting_image_view_delegate.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "chrome/browser/ui/views/bubble_anchor_util_views.h"
#include "chrome/browser/ui/views/location_bar/icon_label_bubble_view.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_strip_top_container.h"
#include "ui/base/interaction/element_identifier.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "url/gurl.h"

class BackForwardButton;
class BrowserWindowInterface;
class ExpandOnHoverLock;
class ReloadButton;
class ToolbarView;
class WebAppContentSettingsContainer;

namespace actions {
class ActionItem;
}  // namespace actions

namespace tabs {
class VerticalTabStripStateController;
}  // namespace tabs

namespace views {
class BubbleAnchor;
class Button;
class ImageButton;
class LabelButton;
class WidgetDelegate;
}  // namespace views

namespace strata {

// The vertical tab strip region's top: upstream's row (traffic lights,
// collapse button, combo button) plus back, forward and reload at the row's
// right end and, under it, a pill with the active tab's host that opens the
// command bar, with a site info button at its left and the location bar's
// content setting icons (blocked pop-ups, a quiet permission prompt, ...) at
// its right. In vertical tab mode it also holds the toolbar's extensions,
// pinned actions and app menu, and the toolbar is hidden. Collapsed, only the
// app menu and the content setting icons show, and a bubble that
// anchors to the extensions or pinned actions expands the sidebar until it
// closes. See docs/design/command-bar.md, "Slice 2" and "Slice 3".
class SidebarHeader : public VerticalTabStripTopContainer,
                      public CommandObserver,
                      public TabStripModelObserver,
                      public IconLabelBubbleView::Delegate,
                      public ContentSettingImageViewDelegate {
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

  // Where bubbles go that upstream anchors to the hidden toolbar: the pill,
  // or the app menu while the sidebar is collapsed. Null if neither is drawn.
  views::View* GetBubbleAnchorView();
  // Where bubbles go that upstream aligns with the location bar's page info
  // icon (permission prompts): the site info button, else the pill. Nullopt
  // while neither is drawn, so upstream falls back to the app menu.
  std::optional<bubble_anchor_util::AnchorConfiguration>
  GetPageInfoAnchorConfiguration();

  // Updates the content setting icons from the active tab, as the location
  // bar updates its own.
  void UpdateContentSettingsIcons();

  // Gives the sidebar pane focus, starting at the back button, as focusing
  // the toolbar does. False if it can't take focus.
  bool FocusToolbarControls();
  // Gives the sidebar pane focus on the app menu button, as focusing the
  // toolbar's app menu does. False if it can't take focus.
  bool FocusAppMenu();

  BackForwardButton* back_button_for_testing() { return back_; }
  BackForwardButton* forward_button_for_testing() { return forward_; }
  ReloadButton* reload_button_for_testing() { return reload_; }
  views::LabelButton* pill_for_testing() { return pill_; }
  views::ImageButton* site_info_button_for_testing() { return site_info_; }
  WebAppContentSettingsContainer* content_settings_for_testing() {
    return content_settings_;
  }

  // VerticalTabStripTopContainer:
  views::ProposedLayout CalculateProposedLayout(
      const views::SizeBounds& size_bounds) const override;
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;
  bool IsPositionInWindowCaption(const gfx::Point& point) override;
  void ViewHierarchyChanged(
      const views::ViewHierarchyChangedDetails& details) override;

  // CommandObserver:
  void EnabledStateChangedForCommand(int id, bool enabled) override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab,
                      TabChangeType change_type) override;

  // IconLabelBubbleView::Delegate:
  SkColor GetIconLabelBubbleSurroundingForegroundColor() const override;
  SkColor GetIconLabelBubbleBackgroundColor() const override;

  // ContentSettingImageViewDelegate:
  bool ShouldHideContentSettingImage() override;
  content::WebContents* GetContentSettingWebContents() override;
  ContentSettingBubbleModelDelegate* GetContentSettingBubbleModelDelegate()
      override;

 private:
  // A toolbar view held here, and where it was in the toolbar.
  struct MovedView {
    raw_ptr<views::View> view;
    size_t toolbar_index;
  };

  // A button in the extensions or pinned actions slot, and whether a bubble
  // or menu anchors to it.
  struct WatchedButton {
    raw_ptr<views::Button> button;
    bool anchored = false;
    base::CallbackListSubscription subscription;
  };

  // Moves the toolbar's extensions, pinned actions and app menu into their
  // slots, or back to the toolbar.
  void TakeToolbarControls();
  void ReturnToolbarControls();
  void OnModeChanged(tabs::VerticalTabStripStateController* controller);

  // Watches the buttons under the extensions and pinned actions slots, minus
  // those under |removed| (a subtree about to leave them).
  void WatchSlotButtons(const views::View* removed);
  void OnAnchorCountChanged(WatchedButton* watched, size_t count);
  // Keeps the sidebar expanded while a watched button is anchored: collapsed,
  // its slot is hidden and the bubble would point at nothing.
  void UpdateExpandLock();

  // Whether the header lays out as collapsed at |width|: only the app menu
  // and the content setting icons show, in a column.
  bool IsLaidOutCollapsed(int width) const;
  // Stacks the content setting icons in a column while laid out collapsed,
  // and in a row in the pill otherwise.
  void UpdateContentSettingsOrientation();
  // Gives the region pane focus, starting at |view|.
  bool SetPaneFocus(views::View* view);

  // Shows the active tab's URL in the pill, its security icon on the site
  // info button and its loading state on reload.
  void UpdateForActiveTab();
  // Makes room in the pill for the site info button and the content setting
  // icons that show.
  void UpdatePillInsets();
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
  // Inside the pill, at its right.
  raw_ptr<WebAppContentSettingsContainer> content_settings_ = nullptr;
  // Whether the active tab has page info to show.
  bool show_site_info_ = false;
  // Hold the toolbar's views while they are here. The layout shows and hides
  // the slots, so upstream keeps control of the views' own visibility.
  raw_ptr<views::View> extensions_slot_ = nullptr;
  raw_ptr<views::View> pinned_actions_slot_ = nullptr;
  raw_ptr<views::View> app_menu_slot_ = nullptr;

  raw_ptr<ToolbarView> toolbar_ = nullptr;
  std::vector<MovedView> moved_views_;
  std::vector<std::unique_ptr<WatchedButton>> watched_buttons_;
  std::unique_ptr<ExpandOnHoverLock> expand_lock_;
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

// Hooks for bubbles (see patches/). |anchor| as upstream picked it, or the
// header's bubble anchor if |anchor| is in a toolbar the header has hidden
// (a location bar icon, say) or is that window's top container (upstream's
// fallback when the location bar isn't drawn). The arrow goes with it: one
// on the pill's left, since the pill is at the window's left.
views::BubbleAnchor GetToolbarBubbleAnchor(const views::BubbleAnchor& anchor);
views::BubbleBorder::Arrow GetToolbarBubbleArrow(
    const views::BubbleAnchor& anchor,
    views::BubbleBorder::Arrow arrow);
// For ChromeViewsDelegate::OnBeforeWidgetInit(), so it covers every bubble,
// however it picked its anchor: a bubble still anchored in a toolbar the
// header has hidden (or its top container) moves to the header's bubble
// anchor, and one anchored in the header opens toward the page, with its
// arrow on the leading side. No-op for other widgets.
void AdjustBubbleForSidebar(views::WidgetDelegate* delegate);
// For bubble_anchor_util::GetPageInfoAnchorConfiguration(): see
// SidebarHeader::GetPageInfoAnchorConfiguration().
std::optional<bubble_anchor_util::AnchorConfiguration>
GetPageInfoAnchorConfiguration(BrowserWindowInterface* browser);
// For LocationBarView::RefreshContentSettingViews(): see
// SidebarHeader::UpdateContentSettingsIcons().
void UpdateContentSettingsIcons(BrowserWindowInterface* browser);
// For ToolbarView::FocusToolbar() (IDC_FOCUS_TOOLBAR, macOS's "Move focus to
// the window toolbar"): focuses the header if it holds the toolbar's
// controls. False to let the toolbar take focus.
bool FocusToolbarControls(BrowserWindowInterface* browser);
// For ToolbarView::SetPaneFocusAndFocusAppMenu() (IDC_FOCUS_MENU_BAR: Alt+F,
// F10; not on macOS): focuses the header's app menu if it holds the
// toolbar's controls. False to let the toolbar take focus.
bool FocusAppMenu(BrowserWindowInterface* browser);

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HEADER_H_
