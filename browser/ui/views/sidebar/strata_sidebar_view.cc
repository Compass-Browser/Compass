// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/strata_sidebar_view.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "content/public/browser/web_contents.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/ui/views/frame/window_frame.h"
#include "strata/browser/ui/views/sidebar/sidebar_context_menu.h"
#include "strata/browser/ui/views/sidebar/sidebar_hover_card.h"
#include "strata/browser/ui/views/sidebar/sidebar_preloader.h"
#include "strata/browser/ui/views/sidebar/sidebar_swap.h"
#include "strata/browser/ui/views/sidebar/sidebar_tint.h"
#include "strata/browser/ui/webui/sidebar/strata_sidebar_ui.h"
#include "strata/browser/window_sync/space_directory.h"
#include "strata/browser/window_sync/window_features.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/accelerators/accelerator_manager.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/controls/webview/web_contents_set_background_color.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/layout/flex_layout_types.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_utils.h"
#include "url/gurl.h"

namespace strata {

namespace {

#if BUILDFLAG(IS_MAC)
constexpr int kModifiers = ui::EF_CONTROL_DOWN;
#else
constexpr int kModifiers = ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN;
#endif

// Takes whatever height the region's content area has left.
views::FlexSpecification SidebarFlex() {
  return views::FlexSpecification(views::LayoutOrientation::kVertical,
                                  views::MinimumFlexSizeRule::kScaleToZero,
                                  views::MaximumFlexSizeRule::kUnbounded)
      .WithWeight(1);
}

}  // namespace

SpaceShortcuts::SpaceShortcuts(BrowserWindowInterface* browser)
    : browser_(browser) {}

SpaceShortcuts::~SpaceShortcuts() {
  Unregister();
}

void SpaceShortcuts::Register(views::FocusManager* focus_manager) {
  Unregister();
  focus_manager_ = focus_manager;
  for (int n = 0; n < 9; ++n) {
    focus_manager_->RegisterAccelerator(
        ui::Accelerator(static_cast<ui::KeyboardCode>(ui::VKEY_1 + n),
                        kModifiers),
        ui::AcceleratorManager::kNormalPriority, this);
  }
}

void SpaceShortcuts::Unregister() {
  if (focus_manager_) {
    focus_manager_->UnregisterAccelerators(this);
    focus_manager_ = nullptr;
  }
}

bool SpaceShortcuts::AcceleratorPressed(const ui::Accelerator& accelerator) {
  WindowFeatures* features = WindowFeatures::From(browser_);
  if (!features || features->is_parking_window()) {
    return false;
  }
  // Counted across profiles, as the switcher shows them.
  const std::vector<SpaceDirectory::Entry> spaces =
      GetSwitchableSpaces(browser_);
  const size_t index = accelerator.key_code() - ui::VKEY_1;
  if (index >= spaces.size()) {
    // Nothing there; let the key go on.
    return false;
  }
  SwitchWindowToSpace(browser_, spaces[index].profile, *spaces[index].space);
  return true;
}

bool SpaceShortcuts::CanHandleAccelerators() const {
  // Also while the sidebar is collapsed.
  return true;
}

StrataSidebarView::StrataSidebarView(BrowserWindowInterface* browser)
    : views::WebView(browser->GetProfile()),
      browser_(browser),
      space_shortcuts_(browser),
      command_bar_(browser) {
  // The profile's preloaded sidebar, if there is one, has its renderer and
  // page loaded already (a switch to another profile's space).
  preloaded_contents_ = SidebarPreloader::Get()->Take(browser->GetProfile());
  if (preloaded_contents_) {
    SetWebContents(preloaded_contents_.get());
  }
  // The page asks for its window as soon as it connects, so set it first. A
  // preloaded page connected without one, and binds now.
  webui::SetBrowserWindowInterface(GetWebContents(), browser);
  InstallSwapPresenter();
  // Let the region's frame-colored background, and the tint over it, show
  // through, so the sidebar follows the theme, the window's active state and
  // its space's color.
  views::WebContentsSetBackgroundColor::CreateForWebContentsWithColor(
      GetWebContents(), SK_ColorTRANSPARENT);
  SidebarHoverCard::CreateForWebContents(GetWebContents(), this);
  SidebarContextMenu::CreateForWebContents(GetWebContents(), this);
  if (!preloaded_contents_) {
    LoadInitialURL(GURL(kStrataSidebarURL));
  }
}

StrataSidebarView::~StrataSidebarView() {
  // The WebView lets go of it before it goes.
  if (preloaded_contents_) {
    SetWebContents(nullptr);
  }
}

void StrataSidebarView::PreloadCommandBar() {
  command_bar_.Preload();
}

void StrataSidebarView::AddedToWidget() {
  views::WebView::AddedToWidget();
  space_shortcuts_.Register(GetFocusManager());
  // So the first Cmd+T opens the bar at once. A swap's window does it once it
  // has taken the old one's place (sidebar_swap.cc).
  if (!IsCreatingSwapWindow()) {
    command_bar_.Preload();
  }
  // So a switch from here to another profile's space finds its sidebar
  // loaded.
  SidebarPreloader::Get()->RefillLater();
}

void StrataSidebarView::RemovedFromWidget() {
  space_shortcuts_.Unregister();
  views::WebView::RemovedFromWidget();
}

BEGIN_METADATA(StrataSidebarView)
END_METADATA

std::unique_ptr<views::View> CreateSidebar(BrowserWindowInterface* browser) {
  return std::make_unique<StrataSidebarView>(browser);
}

void InstallSidebar(views::View* sidebar, views::View* bottom_container) {
  CHECK(views::IsViewClass<StrataSidebarView>(sidebar));
  CHECK(bottom_container);
  // The region gives its tab strip the native strip's flex rule and margins.
  sidebar->SetProperty(views::kFlexBehaviorKey, SidebarFlex());
  sidebar->ClearProperty(views::kMarginsKey);

  // Upstream lets the bottom container take all the space under the strip.
  // FlexLayout only gives zero-preferred-size views (the sidebar's WebView)
  // what is left over, so cap it at its preferred height.
  bottom_container->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::MinimumFlexSizeRule::kPreferred,
                               views::MaximumFlexSizeRule::kPreferred));
  // It only holds upstream's New Tab button, which the page's New Tab row
  // replaces. Hidden, FlexLayout gives it no space.
  bottom_container->SetVisible(false);

  // One color from the header to the bottom edge, not just behind the page.
  views::View* region = sidebar->parent();
  while (region && !views::IsViewClass<VerticalTabStripRegionView>(region)) {
    region = region->parent();
  }
  CHECK(region);
  BrowserWindowInterface* browser =
      views::AsViewClass<StrataSidebarView>(sidebar)->browser();
  // The region adds the sidebar again when vertical tabs come back.
  if (!WindowFrame::FromRegion(region)) {
    SidebarTint::Install(region, browser);
  }
  // The frame around the page, the same color.
  WindowFrame::Install(region, browser);
}

ActiveTabWatcher::ActiveTabWatcher(BrowserWindowInterface* browser,
                                   Callback callback)
    : callback_(std::move(callback)) {
  active_tab_subscription_ =
      browser->RegisterActiveTabDidChange(base::BindRepeating(
          &ActiveTabWatcher::OnActiveTabChanged, base::Unretained(this)));
  OnActiveTabChanged(browser);
}

ActiveTabWatcher::~ActiveTabWatcher() = default;

void ActiveTabWatcher::OnActiveTabChanged(BrowserWindowInterface* browser) {
  tabs::TabInterface* tab = browser->GetActiveTabInterface();
  will_detach_subscription_ =
      tab ? tab->RegisterWillDetach(base::BindRepeating(
                &ActiveTabWatcher::OnWillDetach, base::Unretained(this)))
          : base::CallbackListSubscription();
  callback_.Run(tab);
}

void ActiveTabWatcher::OnWillDetach(tabs::TabInterface* tab,
                                    tabs::TabInterface::DetachReason reason) {
  will_detach_subscription_ = {};
  callback_.Run(nullptr);
}

}  // namespace strata
