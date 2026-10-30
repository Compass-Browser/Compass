// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/frame/window_frame.h"

#include <algorithm>

#include <memory>

#include "base/check.h"
#include "build/build_config.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "strata/browser/ui/views/frame/content_frame.h"
#include "strata/browser/ui/views/sidebar/sidebar_tint.h"
#include "ui/base/class_property.h"
#include "ui/gfx/geometry/point.h"
#include "ui/views/widget/widget.h"

DEFINE_UI_CLASS_PROPERTY_TYPE(strata::WindowFrame*)

namespace strata {

namespace {

DEFINE_OWNED_UI_CLASS_PROPERTY_KEY(WindowFrame, kWindowFrameKey)

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
  SidebarTint::Install(region->parent(), browser);
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

bool IsOnWindowFrame(views::View* browser_view, const gfx::Point& point) {
  // The tint doesn't take events, so only the browser view itself is there.
  return browser_view->GetLocalBounds().Contains(point) &&
         browser_view->GetEventHandlerForPoint(point) == browser_view;
}

#if !BUILDFLAG(IS_MAC)
void SetCaptionButtonsOpacity(views::Widget* widget, float opacity) {}
#endif

}  // namespace strata
