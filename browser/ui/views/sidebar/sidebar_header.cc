// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/sidebar_header.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/functional/bind.h"
#include "base/i18n/rtl.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_element_identifiers.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_content_setting_bubble_model_delegate.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/view_ids.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/top_container_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "chrome/browser/ui/views/page_info/page_info_bubble_specification.h"
#include "chrome/browser/ui/views/page_info/page_info_bubble_view.h"
#include "chrome/browser/ui/views/tabs/shared/tab_strip_combo_button.h"
#include "chrome/browser/ui/views/toolbar/back_forward_button.h"
#include "chrome/browser/ui/views/toolbar/reload_button.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "chrome/browser/ui/views/web_apps/frame_toolbar/web_app_content_settings_container.h"
#include "chrome/common/webui_url_constants.h"
#include "components/tabs/public/tab_interface.h"
#include "components/omnibox/browser/location_bar_model.h"
#include "components/security_state/core/security_state.h"
#include "components/url_formatter/url_formatter.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "strata/browser/ui/views/command_bar/command_bar.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/scoped_layer_animation_settings.h"
#include "ui/gfx/animation/tween.h"
#include "ui/base/window_open_disposition_utils.h"
#include "ui/color/color_provider.h"
#include "ui/events/event.h"
#include "ui/gfx/color_palette.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/accessible_pane_view.h"
#include "ui/views/background.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/proposed_layout.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "url/url_constants.h"

namespace strata {

namespace {

// How long the header takes to settle after a switch to another profile's
// space: the sidebar's slide (SLIDE_MS in the page).
constexpr base::TimeDelta kContinueDuration = base::Milliseconds(280);

// Space between the nav buttons, and between them and their neighbors.
constexpr int kButtonGap = 2;
constexpr int kPillHeight = 32;
// Space above and below the pill.
constexpr int kPillMargin = 6;
constexpr int kPillHorizontalPadding = 12;
// The site info button sits inside the pill, at its left.
constexpr int kSiteInfoSize = 24;
constexpr int kSiteInfoInset = 4;
constexpr int kSiteInfoIconSize = 16;
// Below this the extensions and pinned actions leave the pill's row.
constexpr int kPillMinWidth = 120;

void ExecuteNavCommand(BrowserWindowInterface* browser,
                       int command,
                       const ui::Event& event) {
  chrome::ExecuteCommandWithDisposition(
      browser, command, ui::DispositionFromEventFlags(event.flags()));
}

bool IsNewTabPage(const GURL& url) {
  return url.is_empty() || url == GURL(url::kAboutBlankURL) ||
         (url.SchemeIs(content::kChromeUIScheme) &&
          (url.host() == chrome::kChromeUINewTabHost ||
           url.host() == chrome::kChromeUINewTabPageHost));
}

// The toolbar's child with |id| as its element identifier, or null.
views::View* FindToolbarChild(ToolbarView* toolbar,
                              ui::ElementIdentifier id) {
  for (views::View* child : toolbar->children()) {
    if (child->GetProperty(views::kElementIdentifierKey) == id) {
      return child;
    }
  }
  return nullptr;
}

std::unique_ptr<views::View> CreateSlot() {
  auto slot = std::make_unique<views::View>();
  slot->SetLayoutManager(std::make_unique<views::FillLayout>());
  return slot;
}

// The size of |slot|'s view, empty while upstream hides it (FillLayout
// would still count it).
gfx::Size GetSlotSize(const views::View* slot) {
  if (slot->children().empty() || !slot->children().front()->GetVisible()) {
    return gfx::Size();
  }
  return slot->GetPreferredSize();
}

void CollectButtons(views::View* view, std::vector<views::Button*>& buttons) {
  if (auto* button = views::Button::AsButton(view)) {
    buttons.push_back(button);
  }
  for (views::View* child : view->children()) {
    CollectButtons(child, buttons);
  }
}

// The header of the window |anchor| is in, if it holds that window's toolbar
// controls and |anchor| is in the hidden toolbar or is the top container.
SidebarHeader* GetHeaderForToolbarAnchor(const views::BubbleAnchor& anchor) {
  const views::View* view = anchor.GetIfView();
  if (!view || !view->GetWidget()) {
    return nullptr;
  }
  BrowserView* browser_view = BrowserView::GetBrowserViewForNativeWindow(
      view->GetWidget()->GetNativeWindow());
  if (!browser_view) {
    return nullptr;
  }
  SidebarHeader* header = SidebarHeader::From(browser_view->browser());
  if (!header || !header->HoldsToolbarControls()) {
    return nullptr;
  }
  const bool in_toolbar =
      browser_view->toolbar() && browser_view->toolbar()->Contains(view);
  if (!in_toolbar && view != browser_view->top_container()) {
    return nullptr;
  }
  return header->GetBubbleAnchorView() ? header : nullptr;
}

// The header of the window |view| is in, if it holds the toolbar controls
// and contains |view|.
SidebarHeader* GetHeaderContaining(const views::View* view) {
  if (!view || !view->GetWidget()) {
    return nullptr;
  }
  BrowserView* browser_view = BrowserView::GetBrowserViewForNativeWindow(
      view->GetWidget()->GetNativeWindow());
  SidebarHeader* header =
      browser_view ? SidebarHeader::From(browser_view->browser()) : nullptr;
  return header && header->HoldsToolbarControls() && header->Contains(view)
             ? header
             : nullptr;
}

// |arrow| with the bubble opening toward the window's trailing side, where
// the page is: the arrow on the leading side.
views::BubbleBorder::Arrow GetLeadingArrow(views::BubbleBorder::Arrow arrow) {
  using views::BubbleBorder;
  if (!BubbleBorder::has_arrow(arrow) || BubbleBorder::is_arrow_on_left(arrow)) {
    return arrow;
  }
  switch (arrow) {
    case BubbleBorder::TOP_CENTER:
      return BubbleBorder::TOP_LEFT;
    case BubbleBorder::BOTTOM_CENTER:
      return BubbleBorder::BOTTOM_LEFT;
    default:
      return BubbleBorder::horizontal_mirror(arrow);
  }
}

// The child layout of |view| in |layout|, or null.
const views::ChildLayout* FindChildLayout(const views::ProposedLayout& layout,
                                          const views::View* view) {
  auto it = std::ranges::find(layout.child_layouts, view,
                              &views::ChildLayout::child_view);
  return it == layout.child_layouts.end() ? nullptr : &*it;
}

}  // namespace

DEFINE_USER_DATA(SidebarHeader);
DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(SidebarHeader, kSiteInfoButtonElementId);

SidebarHeader::SidebarHeader(
    tabs::VerticalTabStripStateController* state_controller,
    actions::ActionItem* root_action_item,
    BrowserWindowInterface* browser)
    : VerticalTabStripTopContainer(state_controller, root_action_item, browser),
      state_controller_(state_controller),
      browser_(browser),
      collapse_button_(GetCollapseButton()),
      combo_button_(GetComboButton()),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {
  // In reading order, after upstream's buttons.
  back_ = AddChildView(std::make_unique<BackForwardButton>(
      BackForwardButton::Direction::kBack,
      base::BindRepeating(&ExecuteNavCommand, browser_, IDC_BACK), browser_));
  forward_ = AddChildView(std::make_unique<BackForwardButton>(
      BackForwardButton::Direction::kForward,
      base::BindRepeating(&ExecuteNavCommand, browser_, IDC_FORWARD),
      browser_));
  // No window metrics manager: the toolbar's reload button reports those.
  reload_ = AddChildView(std::make_unique<ReloadButton>(
      browser_->GetProfile(), chrome::BrowserCommandController::From(browser_),
      /*window_metrics_manager=*/nullptr));

  auto pill = std::make_unique<views::LabelButton>(base::BindRepeating(
      &SidebarHeader::OnPillPressed, base::Unretained(this)));
  pill->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  pill->SetBackground(views::CreateRoundedRectBackground(
      kColorLocationBarBackground, kPillHeight / 2));
  pill->SetEnabledTextColors(kColorOmniboxText);
  pill->SetTooltipText(u"Show URL and search");
  pill_ = AddChildView(std::move(pill));

  // After the pill, so it gets events over it.
  auto site_info = views::CreateVectorImageButton(base::BindRepeating(
      &SidebarHeader::OnSiteInfoPressed, base::Unretained(this)));
  site_info->SetTooltipText(u"View site information");
  site_info->SetProperty(views::kElementIdentifierKey,
                         kSiteInfoButtonElementId);
  site_info_ = AddChildView(std::move(site_info));
  // Hidden until an icon shows: an empty container still lays out.
  content_settings_ =
      AddChildView(std::make_unique<WebAppContentSettingsContainer>(
          browser_, this, this));

  extensions_slot_ = AddChildView(CreateSlot());
  pinned_actions_slot_ = AddChildView(CreateSlot());
  app_menu_slot_ = AddChildView(CreateSlot());
  WatchButtons(nullptr);

  mode_subscription_ = state_controller_->RegisterOnModeChanged(
      base::BindRepeating(&SidebarHeader::OnModeChanged,
                          base::Unretained(this)));

  for (int command : {IDC_BACK, IDC_FORWARD, IDC_RELOAD}) {
    chrome::AddCommandObserver(browser_, command, this);
    EnabledStateChangedForCommand(command,
                                  chrome::IsCommandEnabled(browser_, command));
  }
  browser_->GetTabStripModel()->AddObserver(this);
  UpdateForActiveTab();
}

SidebarHeader::~SidebarHeader() {
  // BrowserView gives the controls back first (DetachToolbar()): the toolbar
  // uses them until it is destroyed.
  CHECK(moved_views_.empty());
  watched_buttons_.clear();
  expand_lock_.reset();
  browser_->GetTabStripModel()->RemoveObserver(this);
  for (int command : {IDC_BACK, IDC_FORWARD, IDC_RELOAD}) {
    chrome::RemoveCommandObserver(browser_, command, this);
  }
}

// static
SidebarHeader* SidebarHeader::From(BrowserWindowInterface* browser) {
  return Get(browser->GetUnownedUserDataHost());
}

// static
std::u16string SidebarHeader::GetPillText(const GURL& url) {
  if (IsNewTabPage(url)) {
    return u"Search or enter URL";
  }
  if (url.SchemeIsHTTPOrHTTPS() && url.has_host()) {
    return base::UTF8ToUTF16(url_formatter::StripWWW(std::string(url.host())));
  }
  return base::UTF8ToUTF16(url.spec());
}

views::ProposedLayout SidebarHeader::CalculateProposedLayout(
    const views::SizeBounds& size_bounds) const {
  views::ProposedLayout layout =
      VerticalTabStripTopContainer::CalculateProposedLayout(size_bounds);
  const std::array<views::View*, 3> buttons = {back_.get(), forward_.get(),
                                                reload_.get()};
  const int width = layout.host_size.width();
  int bottom = layout.host_size.height();

  if (IsLaidOutCollapsed(width)) {
    for (views::View* view : {static_cast<views::View*>(back_.get()),
                              static_cast<views::View*>(forward_.get()),
                              static_cast<views::View*>(reload_.get()),
                              static_cast<views::View*>(pill_.get()),
                              static_cast<views::View*>(site_info_.get()),
                              extensions_slot_.get(),
                              pinned_actions_slot_.get()}) {
      layout.child_layouts.emplace_back(view, false, gfx::Rect());
    }
    // The app menu stays reachable, in a row of its own.
    const gfx::Size size = GetSlotSize(app_menu_slot_);
    const bool show_menu = !size.IsEmpty();
    gfx::Rect bounds;
    if (show_menu) {
      bounds = gfx::Rect((width - size.width()) / 2, bottom + kButtonGap,
                         size.width(), size.height());
      layout.host_size.set_height(bounds.bottom() + kButtonGap);
    }
    layout.child_layouts.emplace_back(app_menu_slot_.get(), show_menu,
                                      bounds);
    // The content setting icons stay as indicators, in a column under it
    // (UpdateContentSettingsOrientation()).
    const gfx::Size icons_size = content_settings_->GetPreferredSize();
    const bool show_icons = icons_size.height() > 0;
    gfx::Rect icons_bounds;
    if (show_icons) {
      icons_bounds =
          gfx::Rect((width - icons_size.width()) / 2,
                    layout.host_size.height() + kButtonGap, icons_size.width(),
                    icons_size.height());
      layout.host_size.set_height(icons_bounds.bottom() + kButtonGap);
    }
    layout.child_layouts.emplace_back(content_settings_.get(), show_icons,
                                      icons_bounds);
    return layout;
  }

  // The nav buttons end the top row, left of the combo button if any of it
  // shows, centered on the collapse button.
  int buttons_width = -kButtonGap;
  int buttons_height = 0;
  for (views::View* view : buttons) {
    const gfx::Size size = view->GetPreferredSize();
    buttons_width += size.width() + kButtonGap;
    buttons_height = std::max(buttons_height, size.height());
  }
  const views::ChildLayout* collapse =
      FindChildLayout(layout, collapse_button_);
  const views::ChildLayout* combo = FindChildLayout(layout, combo_button_);
  int left = collapse && collapse->visible ? collapse->bounds.right() : 0;
  int right = width;
  if (combo && combo->visible && !combo->bounds.IsEmpty()) {
    right = combo->bounds.x();
  }
  int center_y =
      collapse && collapse->visible ? collapse->bounds.CenterPoint().y()
                                    : bottom / 2;
  if (right - left - 2 * kButtonGap < buttons_width) {
    // No room: a row of their own.
    right = width;
    center_y = bottom + buttons_height / 2;
    bottom += buttons_height;
  } else {
    right -= kButtonGap;
  }
  int x = right - buttons_width;
  for (views::View* view : buttons) {
    const gfx::Size size = view->GetPreferredSize();
    layout.child_layouts.emplace_back(
        view, true,
        gfx::Rect(x, center_y - size.height() / 2, size.width(),
                  size.height()));
    x += size.width() + kButtonGap;
  }

  // The pill's row: [pill][extensions][pinned actions][app menu], with the
  // middle two on a row of their own when they leave the pill too narrow.
  const int pill_y = bottom + kPillMargin;
  const int pill_center_y = pill_y + kPillHeight / 2;
  // The extensions and pinned actions size themselves to the space their
  // parent says is available (FillLayout: the slot's own size), and drop
  // icons that don't fit. Tell them how much they could have, or they could
  // never grow: the header's width, the extensions sharing it with the
  // pinned actions.
  const gfx::Size pinned_size = GetSlotSize(pinned_actions_slot_);
  const int pinned_available = width;
  const int extensions_available =
      pinned_size.IsEmpty() ? width
                            : width - pinned_size.width() - kButtonGap;
  auto place = [&](views::View* slot, int right_edge, int row_center_y,
                   int available_width) {
    const gfx::Size size = GetSlotSize(slot);
    const bool visible = !size.IsEmpty();
    layout.child_layouts.emplace_back(
        slot, visible,
        visible ? gfx::Rect(right_edge - size.width(),
                            row_center_y - size.height() / 2, size.width(),
                            size.height())
                : gfx::Rect(),
        views::SizeBounds(available_width, {}));
    return visible ? right_edge - size.width() - kButtonGap : right_edge;
  };

  int pill_right = place(app_menu_slot_, width, pill_center_y, width);
  int trailing_width = 0;
  int trailing_height = 0;
  for (views::View* slot : {extensions_slot_.get(), pinned_actions_slot_.get()}) {
    const gfx::Size size = GetSlotSize(slot);
    if (!size.IsEmpty()) {
      trailing_width += size.width() + kButtonGap;
      trailing_height = std::max(trailing_height, size.height());
    }
  }
  int content_bottom = pill_y + kPillHeight;
  if (pill_right - trailing_width >= kPillMinWidth) {
    pill_right = place(pinned_actions_slot_, pill_right, pill_center_y,
                       pinned_available);
    pill_right = place(extensions_slot_, pill_right, pill_center_y,
                       extensions_available);
  } else {
    // A row under the pill, aligned right.
    const int row_center_y =
        content_bottom + kPillMargin + trailing_height / 2;
    place(extensions_slot_,
          place(pinned_actions_slot_, width, row_center_y, pinned_available),
          row_center_y, extensions_available);
    content_bottom += kPillMargin + trailing_height;
  }
  // place() leaves a gap after the last slot it placed.
  const int pill_width = pill_right == width ? width : pill_right + kButtonGap;

  const gfx::Rect pill_bounds(0, pill_y, pill_width, kPillHeight);
  layout.child_layouts.emplace_back(pill_.get(), true, pill_bounds);
  layout.child_layouts.emplace_back(
      site_info_.get(), show_site_info_,
      gfx::Rect(kSiteInfoInset, pill_center_y - kSiteInfoSize / 2,
                kSiteInfoSize, kSiteInfoSize));
  const gfx::Size icons_size = content_settings_->GetPreferredSize();
  layout.child_layouts.emplace_back(
      content_settings_.get(), icons_size.width() > 0,
      gfx::Rect(pill_bounds.right() - kSiteInfoInset - icons_size.width(),
                pill_center_y - icons_size.height() / 2, icons_size.width(),
                icons_size.height()));
  layout.host_size.set_height(content_bottom + kPillMargin);
  if (continue_animation_.is_animating()) {
    // What doesn't fit yet is clipped.
    layout.host_size.set_height(continue_animation_.CurrentValueBetween(
        continue_from_height_, layout.host_size.height()));
  }
  return layout;
}

void SidebarHeader::ContinueFrom(const SidebarHeader& old) {
  if (!gfx::Animation::ShouldRenderRichAnimation()) {
    return;
  }
  continue_from_height_ = old.height();
  continue_animation_.SetSlideDuration(kContinueDuration);
  continue_animation_.SetTweenType(gfx::Tween::EASE_OUT);
  continue_animation_.Reset(0);
  continue_animation_.Show();
  for (views::View* slot : {extensions_slot_.get(),
                            pinned_actions_slot_.get()}) {
    if (!slot) {
      continue;
    }
    slot->SetPaintToLayer();
    slot->layer()->SetFillsBoundsOpaquely(false);
    slot->layer()->SetOpacity(0.f);
    ui::ScopedLayerAnimationSettings settings(slot->layer()->GetAnimator());
    settings.SetTransitionDuration(kContinueDuration);
    settings.SetTweenType(gfx::Tween::EASE_OUT);
    slot->layer()->SetOpacity(1.f);
  }
  PreferredSizeChanged();
}

void SidebarHeader::AnimationProgressed(const gfx::Animation* animation) {
  PreferredSizeChanged();
}

void SidebarHeader::AnimationEnded(const gfx::Animation* animation) {
  for (views::View* slot : {extensions_slot_.get(),
                            pinned_actions_slot_.get()}) {
    if (slot && slot->layer()) {
      slot->DestroyLayer();
    }
  }
  PreferredSizeChanged();
}

void SidebarHeader::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  VerticalTabStripTopContainer::OnBoundsChanged(previous_bounds);
  UpdateContentSettingsOrientation();
}

bool SidebarHeader::IsPositionInWindowCaption(const gfx::Point& point) {
  for (views::View* view :
       {static_cast<views::View*>(back_), static_cast<views::View*>(forward_),
        static_cast<views::View*>(reload_), static_cast<views::View*>(pill_),
        static_cast<views::View*>(site_info_),
        static_cast<views::View*>(content_settings_), extensions_slot_.get(),
        pinned_actions_slot_.get(), app_menu_slot_.get()}) {
    if (view->GetVisible() && view->bounds().Contains(point)) {
      return false;
    }
  }
  return VerticalTabStripTopContainer::IsPositionInWindowCaption(point);
}

void SidebarHeader::ViewHierarchyChanged(
    const views::ViewHierarchyChangedDetails& details) {
  VerticalTabStripTopContainer::ViewHierarchyChanged(details);
  // Called for changes anywhere under the header, before a removal. Once
  // the slots are there, the constructor is done adding buttons.
  if (!extensions_slot_ || !pinned_actions_slot_ || !app_menu_slot_) {
    return;
  }
  if (Contains(details.parent)) {
    WatchButtons(details.is_add ? nullptr : details.child.get());
  }
}

views::View* SidebarHeader::GetBubbleAnchorView() {
  // Collapsed, the pill isn't drawn, but a bubble anchored to it shows the
  // sidebar until it closes (UpdateExpandLock()), and follows the pill there.
  return pill_;
}

std::optional<bubble_anchor_util::AnchorConfiguration>
SidebarHeader::GetPageInfoAnchorConfiguration() {
  if (!HoldsToolbarControls()) {
    return std::nullopt;
  }
  if (site_info_->IsDrawn()) {
    return bubble_anchor_util::AnchorConfiguration{
        views::BubbleAnchor(site_info_.get()), kSiteInfoButtonElementId,
        views::BubbleBorder::TOP_LEFT};
  }
  if (pill_->IsDrawn()) {
    return bubble_anchor_util::AnchorConfiguration{
        views::BubbleAnchor(pill_.get()), std::nullopt,
        views::BubbleBorder::TOP_LEFT};
  }
  return std::nullopt;
}

void SidebarHeader::UpdateContentSettingsIcons() {
  content_settings_->UpdateContentSettingViewsVisibility();
  UpdatePillInsets();
}

bool SidebarHeader::FocusToolbarControls() {
  return HoldsToolbarControls() && SetPaneFocus(back_);
}

bool SidebarHeader::FocusAppMenu() {
  if (!HoldsToolbarControls() || app_menu_slot_->children().empty()) {
    return false;
  }
  return SetPaneFocus(app_menu_slot_->children().front());
}

bool SidebarHeader::IsLaidOutCollapsed(int width) const {
  // Expanded on hover (or held expanded for a bubble), the region is
  // collapsed but as wide as expanded: lay out as expanded, as the page does.
  return state_controller_->IsCollapsed() &&
         width < VerticalTabStripRegionView::kCollapseSnapWidth;
}

void SidebarHeader::UpdateContentSettingsOrientation() {
  const bool collapsed = IsLaidOutCollapsed(width());
  auto* layout =
      static_cast<views::BoxLayout*>(content_settings_->GetLayoutManager());
  const auto orientation = collapsed ? views::BoxLayout::Orientation::kVertical
                                     : views::BoxLayout::Orientation::kHorizontal;
  if (layout->GetOrientation() == orientation) {
    return;
  }
  // Invalidates the layout, so the header is laid out again with the icons'
  // new size.
  layout->SetOrientation(orientation);
  UpdatePillInsets();
}

bool SidebarHeader::SetPaneFocus(views::View* view) {
  // The region is the pane, as F6 finds it: Escape goes back to where focus
  // was, as from the toolbar.
  for (views::View* ancestor = parent(); ancestor;
       ancestor = ancestor->parent()) {
    if (auto* pane = views::AsViewClass<views::AccessiblePaneView>(ancestor)) {
      return pane->SetPaneFocus(view);
    }
  }
  return false;
}

void SidebarHeader::WatchButtons(const views::View* removed) {
  std::vector<views::Button*> buttons;
  CollectButtons(this, buttons);
  if (removed) {
    std::erase_if(buttons, [removed](views::Button* button) {
      return removed->Contains(button);
    });
  }
  std::erase_if(watched_buttons_, [&](const auto& watched) {
    return !std::ranges::contains(buttons, watched->button.get());
  });
  for (views::Button* button : buttons) {
    if (std::ranges::contains(watched_buttons_, button,
                              [](const auto& watched) {
                                return watched->button.get();
                              })) {
      continue;
    }
    auto watched = std::make_unique<WatchedButton>();
    watched->button = button;
    watched->subscription = button->AddAnchorCountChangedCallback(
        base::BindRepeating(&SidebarHeader::OnAnchorCountChanged,
                            base::Unretained(this), watched.get()));
    watched_buttons_.push_back(std::move(watched));
  }
  UpdateExpandLock();
}

void SidebarHeader::OnAnchorCountChanged(WatchedButton* watched,
                                         size_t count) {
  watched->anchored = count > 0;
  UpdateExpandLock();
}

void SidebarHeader::UpdateExpandLock() {
  const bool anchored = std::ranges::any_of(
      watched_buttons_, [](const auto& watched) { return watched->anchored; });
  if (!anchored) {
    expand_lock_.reset();
    return;
  }
  if (expand_lock_) {
    return;
  }
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser_);
  if (browser_view && browser_view->tab_strip_view()) {
    // Expanding on hover ignores the setting under this lock.
    expand_lock_ = browser_view->tab_strip_view()->GetExpandOnHoverLock(
        ExpandOnHoverLockType::kKeepExpanded);
  }
}

void SidebarHeader::EnabledStateChangedForCommand(int id, bool enabled) {
  switch (id) {
    case IDC_BACK:
      back_->SetEnabled(enabled);
      break;
    case IDC_FORWARD:
      forward_->SetEnabled(enabled);
      break;
    case IDC_RELOAD:
      reload_->SetEnabled(enabled);
      break;
  }
}

void SidebarHeader::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  if (selection.active_tab_changed()) {
    UpdateForActiveTab();
  }
}

void SidebarHeader::OnTabChangedAt(tabs::TabInterface* tab,
                                   TabChangeType change_type) {
  if (tab == browser_->GetActiveTabInterface()) {
    UpdateForActiveTab();
  }
}

SkColor SidebarHeader::GetIconLabelBubbleSurroundingForegroundColor() const {
  return GetColorProvider() ? GetColorProvider()->GetColor(kColorOmniboxText)
                            : gfx::kPlaceholderColor;
}

SkColor SidebarHeader::GetIconLabelBubbleBackgroundColor() const {
  return GetColorProvider()
             ? GetColorProvider()->GetColor(kColorLocationBarBackground)
             : gfx::kPlaceholderColor;
}

bool SidebarHeader::ShouldHideContentSettingImage() {
  // The location bar shows them while it has the toolbar's controls.
  return !HoldsToolbarControls();
}

content::WebContents* SidebarHeader::GetContentSettingWebContents() {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  return tab ? tab->GetContents() : nullptr;
}

ContentSettingBubbleModelDelegate*
SidebarHeader::GetContentSettingBubbleModelDelegate() {
  return BrowserContentSettingBubbleModelDelegate::From(browser_);
}

void SidebarHeader::SetToolbar(ToolbarView* toolbar) {
  if (!toolbar) {
    ReturnToolbarControls();
  }
  toolbar_ = toolbar;
  if (toolbar_ && state_controller_->ShouldDisplayVerticalTabs()) {
    TakeToolbarControls();
  }
}

void SidebarHeader::TakeToolbarControls() {
  if (!toolbar_ || HoldsToolbarControls()) {
    return;
  }
  const std::array<std::pair<views::View*, views::View*>, 3> moves = {{
      {FindToolbarChild(toolbar_, kToolbarExtensionsContainerElementId),
       extensions_slot_},
      {FindToolbarChild(toolbar_, kPinnedToolbarActionsContainerElementId),
       pinned_actions_slot_},
      {toolbar_->GetViewByID(VIEW_ID_APP_MENU), app_menu_slot_},
  }};
  for (auto [view, slot] : moves) {
    if (!view || view->parent() != toolbar_) {
      continue;
    }
    moved_views_.push_back({view, toolbar_->GetIndexOf(view).value()});
    slot->AddChildView(view);
  }
  UpdateForActiveTab();
  // Lays out BrowserView too, which shows or hides the toolbar.
  toolbar_->InvalidateLayout();
  PreferredSizeChanged();
}

void SidebarHeader::ReturnToolbarControls() {
  if (!toolbar_ || !HoldsToolbarControls()) {
    return;
  }
  // In reverse, so each index is the one the view had.
  for (auto it = moved_views_.rbegin(); it != moved_views_.rend(); ++it) {
    toolbar_->AddChildViewAt(
        it->view.get(),
        std::min(it->toolbar_index, toolbar_->children().size()));
  }
  moved_views_.clear();
  // Lays out BrowserView too, which shows or hides the toolbar.
  toolbar_->InvalidateLayout();
  PreferredSizeChanged();
}

void SidebarHeader::OnModeChanged(
    tabs::VerticalTabStripStateController* controller) {
  if (controller->ShouldDisplayVerticalTabs()) {
    TakeToolbarControls();
  } else {
    ReturnToolbarControls();
  }
}

void SidebarHeader::UpdateForActiveTab() {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  const GURL url = contents ? contents->GetVisibleURL() : GURL();
  pill_->SetText(GetPillText(url));

  // The location bar's model follows the active tab even while hidden.
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser_);
  LocationBarView* location_bar =
      browser_view ? browser_view->GetLocationBarView() : nullptr;
  show_site_info_ = location_bar && !IsNewTabPage(url);
  if (show_site_info_) {
    const LocationBarModel* model = location_bar->GetLocationBarModel();
    const ui::ColorId color =
        model->GetSecurityLevel() == security_state::DANGEROUS
            ? kColorOmniboxSecurityChipDangerous
            : kColorOmniboxText;
    views::SetImageFromVectorIconWithColor(
        site_info_, model->GetVectorIcon(), kSiteInfoIconSize,
        views::IconColors(color, color));
  }
  UpdateContentSettingsIcons();
  reload_->ChangeMode(contents && contents->IsLoading()
                          ? ReloadButton::Mode::kStop
                          : ReloadButton::Mode::kReload,
                      /*force=*/false);
}

void SidebarHeader::UpdatePillInsets() {
  const int text_left = show_site_info_
                            ? kSiteInfoInset + kSiteInfoSize + kSiteInfoInset
                            : kPillHorizontalPadding;
  const int icons_width = content_settings_->GetPreferredSize().width();
  const int text_right = icons_width > 0
                             ? kSiteInfoInset + icons_width + kSiteInfoInset
                             : kPillHorizontalPadding;
  // A new border lays the header out.
  const gfx::Insets insets = gfx::Insets::TLBR(0, text_left, 0, text_right);
  if (!pill_->GetBorder() || pill_->GetInsets() != insets) {
    pill_->SetBorder(views::CreateEmptyBorder(insets));
  }
}

void SidebarHeader::OnPillPressed() {
  if (CommandBar* bar = CommandBar::From(browser_)) {
    bar->Show(command_bar::mojom::Mode::kCurrentEntry);
  }
}

void SidebarHeader::OnSiteInfoPressed() {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents) {
    return;
  }
  content::NavigationEntry* entry =
      contents->GetController().GetVisibleEntry();
  if (!entry || entry->IsInitialEntry()) {
    return;
  }
  // As ShowPageInfoDialogImpl() does, with this button as the anchor.
  PageInfoBubbleSpecification::Builder builder(
      views::BubbleAnchor(site_info_.get()),
      browser_->GetWindow()->GetNativeWindow(), contents,
      entry->GetVirtualURL());
  views::BubbleDialogDelegateView* bubble =
      PageInfoBubbleView::CreatePageInfoBubble(builder.Build());
  bubble->SetHighlightedElement(kSiteInfoButtonElementId);
  bubble->SetArrow(views::BubbleBorder::TOP_LEFT);
  bubble->GetWidget()->Show();
}

std::unique_ptr<VerticalTabStripTopContainer> CreateSidebarHeader(
    tabs::VerticalTabStripStateController* state_controller,
    actions::ActionItem* root_action_item,
    BrowserWindowInterface* browser) {
  return std::make_unique<SidebarHeader>(state_controller, root_action_item,
                                         browser);
}

views::BubbleAnchor GetToolbarBubbleAnchor(const views::BubbleAnchor& anchor) {
  SidebarHeader* header = GetHeaderForToolbarAnchor(anchor);
  return header ? views::BubbleAnchor(header->GetBubbleAnchorView()) : anchor;
}

views::BubbleBorder::Arrow GetToolbarBubbleArrow(
    const views::BubbleAnchor& anchor,
    views::BubbleBorder::Arrow arrow) {
  return GetHeaderForToolbarAnchor(anchor) ? views::BubbleBorder::TOP_LEFT
                                           : arrow;
}

void AdjustBubbleForSidebar(views::WidgetDelegate* delegate) {
  views::BubbleDialogDelegate* bubble =
      delegate ? delegate->AsBubbleDialogDelegate() : nullptr;
  if (!bubble || !bubble->GetAnchorView()) {
    return;
  }
  const views::BubbleAnchor anchor(bubble->GetAnchorView());
  if (GetHeaderForToolbarAnchor(anchor)) {
    bubble->SetAnchorView(GetToolbarBubbleAnchor(anchor).GetIfView());
  }
  if (!GetHeaderContaining(bubble->GetAnchorView())) {
    return;
  }
  // arrow() is mirrored in RTL, and SetArrow() mirrors it again.
  const views::BubbleBorder::Arrow arrow =
      base::i18n::IsRTL() ? views::BubbleBorder::horizontal_mirror(bubble->arrow())
                          : bubble->arrow();
  const views::BubbleBorder::Arrow leading = GetLeadingArrow(arrow);
  if (leading != arrow) {
    bubble->SetArrow(leading);
  }
}

std::optional<bubble_anchor_util::AnchorConfiguration>
GetPageInfoAnchorConfiguration(BrowserWindowInterface* browser) {
  SidebarHeader* header = browser ? SidebarHeader::From(browser) : nullptr;
  return header ? header->GetPageInfoAnchorConfiguration() : std::nullopt;
}

bool FocusToolbarControls(BrowserWindowInterface* browser) {
  SidebarHeader* header = browser ? SidebarHeader::From(browser) : nullptr;
  return header && header->FocusToolbarControls();
}

bool FocusAppMenu(BrowserWindowInterface* browser) {
  SidebarHeader* header = browser ? SidebarHeader::From(browser) : nullptr;
  return header && header->FocusAppMenu();
}

void UpdateContentSettingsIcons(BrowserWindowInterface* browser) {
  if (SidebarHeader* header = browser ? SidebarHeader::From(browser) : nullptr) {
    header->UpdateContentSettingsIcons();
  }
}

bool ShouldHideToolbar(BrowserWindowInterface* browser) {
  SidebarHeader* header = SidebarHeader::From(browser);
  return header && header->HoldsToolbarControls();
}

void AttachToolbar(BrowserWindowInterface* browser, ToolbarView* toolbar) {
  if (SidebarHeader* header = SidebarHeader::From(browser)) {
    header->SetToolbar(toolbar);
  }
}

void DetachToolbar(BrowserWindowInterface* browser) {
  if (SidebarHeader* header = SidebarHeader::From(browser)) {
    header->SetToolbar(nullptr);
  }
}

BEGIN_METADATA(SidebarHeader)
END_METADATA

}  // namespace strata
