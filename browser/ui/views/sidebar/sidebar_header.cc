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
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/views/tabs/shared/tab_strip_combo_button.h"
#include "chrome/browser/ui/views/toolbar/back_forward_button.h"
#include "chrome/browser/ui/views/toolbar/reload_button.h"
#include "chrome/common/webui_url_constants.h"
#include "components/tabs/public/tab_interface.h"
#include "components/url_formatter/url_formatter.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "strata/browser/ui/views/command_bar/command_bar.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/window_open_disposition_utils.h"
#include "ui/events/event.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/background.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/layout/proposed_layout.h"
#include "url/url_constants.h"

namespace strata {

namespace {

// Space between the nav buttons, and between them and their neighbors.
constexpr int kButtonGap = 2;
constexpr int kPillHeight = 32;
// Space above and below the pill.
constexpr int kPillMargin = 6;
constexpr int kPillHorizontalPadding = 12;

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

// The child layout of |view| in |layout|, or null.
const views::ChildLayout* FindChildLayout(const views::ProposedLayout& layout,
                                          const views::View* view) {
  auto it = std::ranges::find(layout.child_layouts, view,
                              &views::ChildLayout::child_view);
  return it == layout.child_layouts.end() ? nullptr : &*it;
}

}  // namespace

SidebarHeader::SidebarHeader(
    tabs::VerticalTabStripStateController* state_controller,
    actions::ActionItem* root_action_item,
    BrowserWindowInterface* browser)
    : VerticalTabStripTopContainer(state_controller, root_action_item, browser),
      state_controller_(state_controller),
      browser_(browser),
      collapse_button_(GetCollapseButton()),
      combo_button_(GetComboButton()) {
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
  pill->SetBorder(views::CreateEmptyBorder(
      gfx::Insets::VH(0, kPillHorizontalPadding)));
  pill->SetBackground(views::CreateRoundedRectBackground(
      kColorLocationBarBackground, kPillHeight / 2));
  pill->SetEnabledTextColors(kColorOmniboxText);
  pill->SetTooltipText(u"Show URL and search");
  pill_ = AddChildView(std::move(pill));

  for (int command : {IDC_BACK, IDC_FORWARD, IDC_RELOAD}) {
    chrome::AddCommandObserver(browser_, command, this);
    EnabledStateChangedForCommand(command,
                                  chrome::IsCommandEnabled(browser_, command));
  }
  browser_->GetTabStripModel()->AddObserver(this);
  UpdateForActiveTab();
}

SidebarHeader::~SidebarHeader() {
  browser_->GetTabStripModel()->RemoveObserver(this);
  for (int command : {IDC_BACK, IDC_FORWARD, IDC_RELOAD}) {
    chrome::RemoveCommandObserver(browser_, command, this);
  }
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

  if (state_controller_->IsCollapsed()) {
    for (views::View* view : buttons) {
      layout.child_layouts.emplace_back(view, false, gfx::Rect());
    }
    layout.child_layouts.emplace_back(pill_.get(), false, gfx::Rect());
    return layout;
  }

  const int width = layout.host_size.width();
  int bottom = layout.host_size.height();

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

  const gfx::Rect pill_bounds(0, bottom + kPillMargin, width, kPillHeight);
  layout.child_layouts.emplace_back(pill_.get(), true, pill_bounds);
  layout.host_size.set_height(pill_bounds.bottom() + kPillMargin);
  return layout;
}

bool SidebarHeader::IsPositionInWindowCaption(const gfx::Point& point) {
  for (views::View* view :
       {static_cast<views::View*>(back_), static_cast<views::View*>(forward_),
        static_cast<views::View*>(reload_), static_cast<views::View*>(pill_)}) {
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

void SidebarHeader::UpdateForActiveTab() {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  pill_->SetText(GetPillText(contents ? contents->GetVisibleURL() : GURL()));
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

std::unique_ptr<VerticalTabStripTopContainer> CreateSidebarHeader(
    tabs::VerticalTabStripStateController* state_controller,
    actions::ActionItem* root_action_item,
    BrowserWindowInterface* browser) {
  return std::make_unique<SidebarHeader>(state_controller, root_action_item,
                                         browser);
}

BEGIN_METADATA(SidebarHeader)
END_METADATA

}  // namespace strata
