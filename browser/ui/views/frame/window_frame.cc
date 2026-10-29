// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/frame/window_frame.h"

#include <memory>

#include "base/check.h"
#include "build/build_config.h"
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
    : region_(region) {
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
  // (empty, it's yet to be laid out).
  const bool hidden = region_->GetVisible() && region_->width() > 0 &&
                      region_->width() <= kCollapsedSidebarWidth &&
                      !widget->IsFullscreen();
  if (hidden == caption_buttons_hidden_) {
    return;
  }
  caption_buttons_hidden_ = hidden;
  SetCaptionButtonsHidden(widget, hidden);
}

bool IsOnWindowFrame(views::View* browser_view, const gfx::Point& point) {
  // The tint doesn't take events, so only the browser view itself is there.
  return browser_view->GetLocalBounds().Contains(point) &&
         browser_view->GetEventHandlerForPoint(point) == browser_view;
}

#if !BUILDFLAG(IS_MAC)
void SetCaptionButtonsHidden(views::Widget* widget, bool hidden) {}
#endif

}  // namespace strata
