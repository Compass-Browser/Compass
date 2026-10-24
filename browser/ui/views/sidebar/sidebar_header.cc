// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/sidebar_header.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_element_identifiers.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/view_ids.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "chrome/browser/ui/views/page_info/page_info_bubble_specification.h"
#include "chrome/browser/ui/views/page_info/page_info_bubble_view.h"
#include "chrome/browser/ui/views/tabs/shared/tab_strip_combo_button.h"
#include "chrome/browser/ui/views/toolbar/back_forward_button.h"
#include "chrome/browser/ui/views/toolbar/reload_button.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
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
#include "ui/base/window_open_disposition_utils.h"
#include "ui/events/event.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/background.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/proposed_layout.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"
#include "url/url_constants.h"

namespace strata {

namespace {

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

  extensions_slot_ = AddChildView(CreateSlot());
  pinned_actions_slot_ = AddChildView(CreateSlot());
  app_menu_slot_ = AddChildView(CreateSlot());

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

  if (state_controller_->IsCollapsed()) {
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
  auto place = [&](views::View* slot, int right_edge, int row_center_y) {
    const gfx::Size size = GetSlotSize(slot);
    const bool visible = !size.IsEmpty();
    layout.child_layouts.emplace_back(
        slot, visible,
        visible ? gfx::Rect(right_edge - size.width(),
                            row_center_y - size.height() / 2, size.width(),
                            size.height())
                : gfx::Rect());
    return visible ? right_edge - size.width() - kButtonGap : right_edge;
  };

  int pill_right = place(app_menu_slot_, width, pill_center_y);
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
    pill_right = place(pinned_actions_slot_, pill_right, pill_center_y);
    pill_right = place(extensions_slot_, pill_right, pill_center_y);
  } else {
    // A row under the pill, aligned right.
    const int row_center_y =
        content_bottom + kPillMargin + trailing_height / 2;
    place(extensions_slot_,
          place(pinned_actions_slot_, width, row_center_y), row_center_y);
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
  layout.host_size.set_height(content_bottom + kPillMargin);
  return layout;
}

bool SidebarHeader::IsPositionInWindowCaption(const gfx::Point& point) {
  for (views::View* view :
       {static_cast<views::View*>(back_), static_cast<views::View*>(forward_),
        static_cast<views::View*>(reload_), static_cast<views::View*>(pill_),
        static_cast<views::View*>(site_info_), extensions_slot_.get(),
        pinned_actions_slot_.get(), app_menu_slot_.get()}) {
    if (view->GetVisible() && view->bounds().Contains(point)) {
      return false;
    }
  }
  return VerticalTabStripTopContainer::IsPositionInWindowCaption(point);
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
  // The border changes with |show_site_info_|, which lays the header out.
  const int text_left = show_site_info_
                            ? kSiteInfoInset + kSiteInfoSize + kSiteInfoInset
                            : kPillHorizontalPadding;
  if (!pill_->GetBorder() || pill_->GetInsets().left() != text_left) {
    pill_->SetBorder(views::CreateEmptyBorder(
        gfx::Insets::TLBR(0, text_left, 0, kPillHorizontalPadding)));
  }
  reload_->ChangeMode(contents && contents->IsLoading()
                          ? ReloadButton::Mode::kStop
                          : ReloadButton::Mode::kReload,
                      /*force=*/false);
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
