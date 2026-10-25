// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/sidebar_tint.h"

#include <memory>

#include "cc/paint/paint_flags.h"
#include "chrome/browser/ui/views/frame/custom_corners_background.h"
#include "strata/browser/window_sync/space_service.h"
#include "third_party/skia/include/core/SkPath.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/view_class_properties.h"

namespace strata {

namespace {

// How much of the space color shows over the frame color.
constexpr SkAlpha kTintAlpha = 0x24;  // 14%

}  // namespace

std::optional<SkColor> GetSpaceTint(SpaceColor color) {
  switch (color) {
    case SpaceColor::kGrey:
      return std::nullopt;
    case SpaceColor::kBlue:
      return SkColorSetRGB(59, 130, 246);
    case SpaceColor::kPurple:
      return SkColorSetRGB(139, 92, 246);
    case SpaceColor::kPink:
      return SkColorSetRGB(236, 72, 153);
    case SpaceColor::kRed:
      return SkColorSetRGB(239, 68, 68);
    case SpaceColor::kOrange:
      return SkColorSetRGB(249, 115, 22);
    case SpaceColor::kYellow:
      return SkColorSetRGB(234, 179, 8);
    case SpaceColor::kGreen:
      return SkColorSetRGB(34, 197, 94);
  }
  return std::nullopt;
}

SidebarTint::SidebarTint(BrowserWindowInterface* browser)
    : features_(WindowFeatures::From(browser)) {
  SetProperty(views::kViewIgnoredByLayoutKey, true);
  SetCanProcessEventsWithinSubtree(false);
  GetViewAccessibility().SetIsIgnored(true);
  if (features_) {
    features_observation_.Observe(features_.get());
    space_list_observation_.Observe(&features_->service().space_list());
  }
}

SidebarTint::~SidebarTint() = default;

// static
SidebarTint* SidebarTint::Install(views::View* region,
                                  BrowserWindowInterface* browser) {
  auto* tint = region->AddChildViewAt(std::make_unique<SidebarTint>(browser),
                                      0);
  tint->region_observation_.Observe(region);
  tint->SetBoundsRect(region->GetLocalBounds());
  return tint;
}

std::optional<SkColor> SidebarTint::GetTintForTesting() const {
  if (!features_) {
    return std::nullopt;
  }
  const std::optional<SkColor> color =
      GetSpaceTint(features_->space_model().color());
  return color ? std::optional(SkColorSetA(*color, kTintAlpha)) : std::nullopt;
}

void SidebarTint::OnPaint(gfx::Canvas* canvas) {
  const std::optional<SkColor> tint = GetTintForTesting();
  if (!tint) {
    return;
  }
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(*tint);
  // In the region's coordinates, which are ours: the region's background
  // has custom corners.
  const views::Background* background = parent()->background();
  const auto* corners =
      background ? background->AsA<CustomCornersBackground>() : nullptr;
  if (corners) {
    canvas->DrawPath(corners->GetBackgroundPath(), flags);
  } else {
    canvas->DrawRect(gfx::RectF(GetLocalBounds()), flags);
  }
}

void SidebarTint::OnViewBoundsChanged(views::View* observed_view) {
  SetBoundsRect(observed_view->GetLocalBounds());
}

void SidebarTint::OnViewIsDeleting(views::View* observed_view) {
  region_observation_.Reset();
}

void SidebarTint::OnSpaceSwitched() {
  SchedulePaint();
}

void SidebarTint::OnSpaceUpdated(const SpaceModel& space) {
  if (features_ && &space == &features_->space_model()) {
    SchedulePaint();
  }
}

BEGIN_METADATA(SidebarTint)
END_METADATA

}  // namespace strata
