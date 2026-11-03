// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/sidebar/compass_sidebar_view.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/frame_separator.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "content/public/browser/navigation_controller.h"
#include "compass/browser/ui/views/frame/content_frame.h"
#include "ui/base/class_property.h"
#include "ui/compositor/layer.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_controller.h"
#include "compass/browser/ui/webui/sidebar/sidebar_page_handler.h"
#include "ui/base/page_transition_types.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/ui/arc_import/arc_importer.h"
#include "compass/browser/ui/views/frame/window_frame.h"
#include "compass/browser/ui/views/sidebar/sidebar_context_menu.h"
#include "compass/browser/ui/views/sidebar/sidebar_hover_card.h"
#include "compass/browser/ui/views/sidebar/sidebar_swap.h"
#include "compass/browser/ui/views/sidebar/sidebar_tint.h"
#include "compass/browser/ui/webui/sidebar/compass_sidebar_ui.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/window_features.h"
#include "third_party/blink/public/common/input/web_gesture_event.h"
#include "third_party/blink/public/common/input/web_input_event.h"
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

namespace compass {

namespace {

// Takes whatever height the region's content area has left.
views::FlexSpecification SidebarFlex() {
  return views::FlexSpecification(views::LayoutOrientation::kVertical,
                                  views::MinimumFlexSizeRule::kScaleToZero,
                                  views::MaximumFlexSizeRule::kUnbounded)
      .WithWeight(1);
}

}  // namespace

SpaceShortcuts::SpaceShortcuts(BrowserWindowInterface* browser)
    : browser_(browser) {
  settings_observation_.Observe(&ShortcutSettings::Get());
}

SpaceShortcuts::~SpaceShortcuts() {
  Unregister();
}

void SpaceShortcuts::Register(views::FocusManager* focus_manager) {
  Unregister();
  focus_manager_ = focus_manager;
  for (int n = 0; n < kSpaceShortcutCount; ++n) {
    for (const ui::Accelerator& accelerator : GetSpaceAccelerators(n)) {
      focus_manager_->RegisterAccelerator(
          accelerator, ui::AcceleratorManager::kNormalPriority, this);
    }
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
  size_t index = spaces.size();
  for (int n = 0; n < kSpaceShortcutCount; ++n) {
    if (std::ranges::contains(GetSpaceAccelerators(n), accelerator)) {
      index = n;
      break;
    }
  }
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

void SpaceShortcuts::OnShortcutsChanged() {
  if (focus_manager_) {
    Register(focus_manager_);
  }
}

CompassSidebarView::CompassSidebarView(BrowserWindowInterface* browser)
    // It owns its page itself, to hand it over.
    : views::WebView(nullptr),
      browser_(browser),
      space_shortcuts_(browser),
      command_bar_(browser),
      peek_(browser),
      claude_panel_(browser) {
  InstallSwapPresenter();
  // A swap's window takes the page of the window it replaces
  // (sidebar_swap.cc).
  if (IsCreatingSwapWindow()) {
    return;
  }
  LoadPage();
  // On the first run, the import from Arc; once the window is built (its
  // WindowFeatures too).
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<BrowserWindowInterface> browser) {
                       if (browser) {
                         arc::MaybeOfferImportOnFirstRun(browser.get());
                       }
                     },
                     browser->GetWeakPtr()));
}

CompassSidebarView::~CompassSidebarView() {
  // The WebView lets go of it before it goes.
  Detach();
}

void CompassSidebarView::PreloadCommandBar() {
  command_bar_.Preload();
}

void CompassSidebarView::LoadPage() {
  if (contents_) {
    return;
  }
  const GURL url(kCompassSidebarURL);
  Profile* profile = browser_->GetProfile();
  content::WebContents::CreateParams params(profile);
  params.site_instance = content::SiteInstance::CreateForURL(profile, url);
  Attach(content::WebContents::Create(params));
  contents_->GetController().LoadURL(url, content::Referrer(),
                                     ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                     std::string());
}

void CompassSidebarView::TakePageFrom(CompassSidebarView& other) {
  if (std::unique_ptr<content::WebContents> contents = other.Detach()) {
    Detach();
    Attach(std::move(contents));
  }
}

void CompassSidebarView::OnProfileWillBeDestroyed(Profile* profile) {
  // The page's profile goes: one of the window's takes its place.
  Detach();
  LoadPage();
}

void CompassSidebarView::Attach(std::unique_ptr<content::WebContents> contents) {
  contents_ = std::move(contents);
  content::WebContents* web_contents = contents_.get();
  // As the WebView does with its own: keys the page doesn't handle go to the
  // window.
  web_contents->SetDelegate(this);
  // Let the region's frame-colored background, and the tint over it, show
  // through, so the sidebar follows the theme, the window's active state and
  // its space's color.
  views::WebContentsSetBackgroundColor::CreateForWebContentsWithColor(
      web_contents, SK_ColorTRANSPARENT);
  SidebarHoverCard::CreateForWebContents(web_contents, this);
  SidebarContextMenu::CreateForWebContents(web_contents, this);
  SetWebContents(web_contents);
  Profile* page_profile =
      Profile::FromBrowserContext(web_contents->GetBrowserContext());
  if (page_profile != browser_->GetProfile()) {
    page_profile_observation_.Observe(page_profile);
  }
  // The page asks for its window as soon as it connects, and a page that
  // moved here binds to this window now (CompassSidebarUI).
  webui::SetBrowserWindowInterface(web_contents, browser_);
}

std::unique_ptr<content::WebContents> CompassSidebarView::Detach() {
  if (!contents_) {
    return nullptr;
  }
  page_profile_observation_.Reset();
  SetWebContents(nullptr);
  contents_->SetDelegate(nullptr);
  // Made for this view.
  contents_->RemoveUserData(SidebarHoverCard::UserDataKey());
  contents_->RemoveUserData(SidebarContextMenu::UserDataKey());
  return std::move(contents_);
}

void CompassSidebarView::AddedToWidget() {
  views::WebView::AddedToWidget();
  space_shortcuts_.Register(GetFocusManager());
  // So the first Cmd+T opens the bar at once. A swap's window does it once it
  // has taken the old one's place (sidebar_swap.cc).
  if (!IsCreatingSwapWindow()) {
    command_bar_.Preload();
  }
}

void CompassSidebarView::RemovedFromWidget() {
  space_shortcuts_.Unregister();
  views::WebView::RemovedFromWidget();
}

bool CompassSidebarView::PreHandleGestureEvent(
    content::WebContents* source,
    const blink::WebGestureEvent& event) {
  using Type = blink::WebInputEvent::Type;
  using Phase = blink::WebGestureEvent::InertialPhaseState;
  if (blink::WebInputEvent::IsPinchGestureEventType(event.GetType()) ||
      event.GetType() == Type::kGestureDoubleTap) {
    return true;
  }
  if (event.SourceDevice() != blink::WebGestureDevice::kTouchpad) {
    return false;
  }
  auto send = [&](sidebar::mojom::SwipePhase phase, float delta_x) {
    content::WebUI* web_ui = source->GetWebUI();
    CompassSidebarUI* ui =
        web_ui ? web_ui->GetController()->GetAs<CompassSidebarUI>() : nullptr;
    if (ui && ui->page_handler()) {
      ui->page_handler()->SendSwipe(phase, delta_x);
    }
  };
  // A scroll's begin and end always go on to the page, even a swipe's: the
  // input router counts a scroll as started only once its begin reaches the
  // page. Without it, the wheel queue sent another begin before every update
  // and never an end, so a swipe restarted at each event and never ended.
  // The page gets no update in between, so nothing scrolls.
  switch (event.GetType()) {
    case Type::kGestureScrollBegin: {
      const auto& begin = event.data.scroll_begin;
      if (swallowing_momentum_ && begin.inertial_phase == Phase::kMomentum) {
        return false;
      }
      swallowing_momentum_ = false;
      // After the trackpad's axis lock, which zeroes the other axis.
      swiping_ = std::abs(begin.delta_x_hint) > std::abs(begin.delta_y_hint);
      if (swiping_) {
        send(sidebar::mojom::SwipePhase::kBegin, 0);
        if (begin.delta_x_hint) {
          send(sidebar::mojom::SwipePhase::kUpdate, begin.delta_x_hint);
        }
      }
      return false;
    }
    case Type::kGestureScrollUpdate:
      if (swiping_) {
        send(sidebar::mojom::SwipePhase::kUpdate,
             event.data.scroll_update.delta_x);
        return true;
      }
      return swallowing_momentum_ &&
             event.data.scroll_update.inertial_phase == Phase::kMomentum;
    case Type::kGestureScrollEnd:
    case Type::kGestureFlingStart:
    case Type::kGestureFlingCancel:
      if (swiping_) {
        swiping_ = false;
        swallowing_momentum_ = true;
        send(sidebar::mojom::SwipePhase::kEnd, 0);
        return event.GetType() != Type::kGestureScrollEnd;
      }
      if (swallowing_momentum_ && event.GetType() == Type::kGestureScrollEnd &&
          event.data.scroll_end.inertial_phase == Phase::kMomentum) {
        // The swipe's momentum is over.
        swallowing_momentum_ = false;
      }
      return false;
    default:
      return false;
  }
}

BEGIN_METADATA(CompassSidebarView)
END_METADATA

std::unique_ptr<views::View> CreateSidebar(BrowserWindowInterface* browser) {
  return std::make_unique<CompassSidebarView>(browser);
}

namespace {

// Set on views that FadeSidebarContents() gave a layer, which it takes back.
DEFINE_UI_CLASS_PROPERTY_KEY(bool, kHasFadeLayerKey, false)

}  // namespace

int GetSidebarSlideOffset(int width, int expanded_width, bool animating) {
  return animating ? std::max(0, expanded_width - width) : 0;
}

void FadeSidebarContents(std::initializer_list<views::View*> views,
                         int width,
                         int expanded_width,
                         bool closing) {
  const int range = expanded_width - kCollapsedSidebarWidth;
  const float opacity =
      closing && range > 0
          ? std::clamp(static_cast<float>(width - kCollapsedSidebarWidth) /
                           range,
                       0.f, 1.f)
          : 1.f;
  for (views::View* view : views) {
    if (!view) {
      continue;
    }
    if (opacity == 1.f && view->GetProperty(kHasFadeLayerKey)) {
      view->DestroyLayer();
      view->ClearProperty(kHasFadeLayerKey);
      continue;
    }
    if (opacity < 1.f && !view->layer()) {
      view->SetPaintToLayer();
      view->layer()->SetFillsBoundsOpaquely(false);
      view->SetProperty(kHasFadeLayerKey, true);
    }
    if (view->layer()) {
      view->layer()->SetOpacity(opacity);
    }
  }
}

void InstallSidebar(views::View* sidebar, views::View* bottom_container) {
  CHECK(views::IsViewClass<CompassSidebarView>(sidebar));
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
  // Arc has no line under the header: hide upstream's, between the top
  // buttons and the strip.
  for (views::View* area : region->children()) {
    for (views::View* child : area->children()) {
      if (views::IsViewClass<FrameSeparator>(child)) {
        child->SetVisible(false);
      }
    }
  }
  BrowserWindowInterface* browser =
      views::AsViewClass<CompassSidebarView>(sidebar)->browser();
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

}  // namespace compass
