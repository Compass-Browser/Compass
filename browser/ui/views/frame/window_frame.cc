// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/frame/window_frame.h"

#include <algorithm>

#include <memory>

#include "base/check.h"
#include "build/build_config.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/view_ids.h"
#include "compass/browser/ui/views/find_bar/find_bar.h"
#include "compass/browser/ui/views/frame/content_frame.h"
#include "compass/browser/ui/views/frame/load_bar.h"
#include "compass/browser/ui/views/frame/space_fade.h"
#include "compass/browser/ui/views/sidebar/sidebar_tint.h"
#include "ui/base/class_property.h"
#include "ui/gfx/geometry/point.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/non_client_view.h"

DEFINE_UI_CLASS_PROPERTY_TYPE(compass::WindowFrame*)

namespace compass {

namespace {

DEFINE_OWNED_UI_CLASS_PROPERTY_KEY(WindowFrame, kWindowFrameKey)

// On the browser view while its frame is translucent.
DEFINE_UI_CLASS_PROPERTY_KEY(bool, kFrameTranslucentKey, false)

#if BUILDFLAG(IS_WIN)
void InvalidateCaptionButtons(views::View* view) {
  switch (view->GetID()) {
    case VIEW_ID_MINIMIZE_BUTTON:
    case VIEW_ID_MAXIMIZE_BUTTON:
    case VIEW_ID_RESTORE_BUTTON:
    case VIEW_ID_CLOSE_BUTTON:
      view->InvalidateLayout();
      return;
    default:
      break;
  }
  for (views::View* child : view->children()) {
    InvalidateCaptionButtons(child);
  }
}
#endif

}  // namespace

// static
void WindowFrame::Install(views::View* region,
                          BrowserWindowInterface* browser) {
  // The region adds the sidebar again when vertical tabs come back.
  if (FromRegion(region)) {
    return;
  }
  region->SetProperty(kWindowFrameKey,
                      std::make_unique<WindowFrame>(region, browser));
}

// static
WindowFrame* WindowFrame::FromRegion(views::View* region) {
  return region->GetProperty(kWindowFrameKey);
}

WindowFrame::WindowFrame(views::View* region, BrowserWindowInterface* browser)
    : region_(region), browser_(browser) {
  // The browser view holds the region and the page; under both, the tint
  // colors the frame as the region's colors the sidebar.
  CHECK(region->parent());
  SidebarTint::Install(region->parent(), browser, /*window_frame=*/true);
  // The page's load progress, in the frame above it.
  LoadBar::Install(region->parent(), browser);
  // Cmd+F, over the page's top trailing corner.
  FindBar::Install(region->parent(), browser);
  // The pages fade from one space to the next.
  space_fade_ = std::make_unique<SpaceFade>(region->parent(), browser);
  region_observation_.Observe(region);
}

WindowFrame::~WindowFrame() = default;

void WindowFrame::OnViewBoundsChanged(views::View* observed_view) {
  UpdateCaptionButtons();
}

void WindowFrame::OnViewVisibilityChanged(views::View* observed_view,
                                          views::View* starting_view,
                                          bool visible) {
  UpdateCaptionButtons();
}

void WindowFrame::OnViewIsDeleting(views::View* observed_view) {
  region_observation_.Reset();
  region_ = nullptr;
}

void WindowFrame::UpdateCaptionButtons() {
  views::Widget* widget = region_ ? region_->GetWidget() : nullptr;
  if (!widget) {
    return;
  }
#if BUILDFLAG(IS_WIN)
  // The frame view may have sized its buttons before this frame was
  // installed: they ask again (GetCompactCaptionButtonSize()).
  if (sized_caption_buttons_for_ != widget) {
    sized_caption_buttons_for_ = widget;
    if (widget->non_client_view() && widget->non_client_view()->frame_view()) {
      InvalidateCaptionButtons(widget->non_client_view()->frame_view());
    }
  }
#endif
  // The region is laid out this narrow only collapsed and not shown on hover
  // (empty, it's yet to be laid out). In between, it's opening or closing.
  float opacity = 1.f;
  if (region_->GetVisible() && region_->width() > 0 &&
      !widget->IsFullscreen()) {
    const tabs::VerticalTabStripStateController* controller =
        tabs::VerticalTabStripStateController::From(browser_);
    const int range =
        (controller ? controller->GetUncollapsedWidth() : 0) -
        kCollapsedSidebarWidth;
    opacity = range > 0 ? std::clamp(static_cast<float>(region_->width() -
                                                        kCollapsedSidebarWidth) /
                                         range,
                                     0.f, 1.f)
                        : (region_->width() <= kCollapsedSidebarWidth ? 0.f
                                                                       : 1.f);
  }
  if (opacity == caption_buttons_opacity_) {
    return;
  }
  caption_buttons_opacity_ = opacity;
  caption_buttons_hidden_ = opacity == 0.f;
  SetCaptionButtonsOpacity(widget, opacity);
}

namespace {

bool HasWindowFrame(views::View* browser_view) {
  return std::ranges::any_of(browser_view->children(), [](views::View* child) {
    return WindowFrame::FromRegion(child) != nullptr;
  });
}

}  // namespace

std::optional<int> GetWindowTitlebarHeight(views::View* browser_view) {
  if (!browser_view || !browser_view->GetWidget() ||
      browser_view->GetWidget()->IsFullscreen() ||
      !HasWindowFrame(browser_view)) {
    return std::nullopt;
  }
  return 2 * kCaptionButtonsCenterY;
}

std::optional<gfx::Size> GetCompactCaptionButtonSize(
    views::View* browser_view) {
  if (!browser_view || !HasWindowFrame(browser_view)) {
    return std::nullopt;
  }
  return kCompactCaptionButtonSize;
}

bool IsOnWindowFrame(views::View* browser_view, const gfx::Point& point) {
  // The tint doesn't take events, so only the browser view itself is there.
  return browser_view->GetLocalBounds().Contains(point) &&
         browser_view->GetEventHandlerForPoint(point) == browser_view;
}

#if !BUILDFLAG(IS_MAC)
void SetCaptionButtonsOpacity(views::Widget* widget, float opacity) {}
#endif

bool IsFrameTranslucent(const views::View* browser_view) {
  return browser_view && browser_view->GetProperty(kFrameTranslucentKey) &&
         browser_view->GetWidget() &&
         !browser_view->GetWidget()->IsFullscreen();
}

void SetFrameTranslucent(views::View* browser_view, bool translucent) {
  views::Widget* widget = browser_view->GetWidget();
  if (!widget || browser_view->GetProperty(kFrameTranslucentKey) ==
                     translucent) {
    return;
  }
  browser_view->SetProperty(kFrameTranslucentKey, translucent);
  // After the property: on Windows, the compositor asks whether to clear to
  // transparent.
  if (translucent) {
    SetWindowBackdrop(widget, true);
  }
  // Upstream's layout picks the region's and the card's corners' frame color
  // (patches/...browser_view_tabbed_layout_impl.cc.patch), and the frame view
  // paints the frame color or not.
  browser_view->InvalidateLayout();
  browser_view->SchedulePaint();
  if (widget->non_client_view() && widget->non_client_view()->frame_view()) {
    widget->non_client_view()->frame_view()->SchedulePaint();
  }
}

void RemoveWindowBackdrop(views::Widget* widget) {
  SetWindowBackdrop(widget, false);
}

#if !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_WIN)
bool IsWindowBackdropSupported() {
  return false;
}

void SetWindowBackdrop(views::Widget* widget, bool on) {}
#endif

#if !BUILDFLAG(IS_MAC)
bool IsBubbleBackdropSupported() {
  return false;
}

void AddBubbleBackdrop(views::Widget* widget,
                       const gfx::Rect& bounds,
                       int corner_radius) {}
#endif

}  // namespace compass
