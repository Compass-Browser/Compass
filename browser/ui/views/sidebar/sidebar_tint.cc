// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/sidebar_tint.h"

#include <memory>

#include "cc/paint/paint_flags.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/custom_corners_background.h"
#include "strata/browser/window_sync/space_service.h"
#include "third_party/skia/include/core/SkPath.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/animation/tween.h"
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

// The fade between two spaces' colors; the page's slide takes as long.
constexpr base::TimeDelta kFadeDuration = base::Milliseconds(280);

SkColor ToTintColor(SpaceColor color) {
  const std::optional<SkColor> tint = GetSpaceTint(color);
  return tint ? SkColorSetA(*tint, kTintAlpha) : SK_ColorTRANSPARENT;
}

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
    : browser_(browser),
      features_(WindowFeatures::From(browser)),
      animation_(kFadeDuration,
                 gfx::LinearAnimation::kDefaultFrameRate,
                 this) {
  SetProperty(views::kViewIgnoredByLayoutKey, true);
  SetCanProcessEventsWithinSubtree(false);
  GetViewAccessibility().SetIsIgnored(true);
  if (features_) {
    features_observation_.Observe(features_.get());
    space_list_observation_.Observe(&features_->service().space_list());
    color_ = GetTargetColor();
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
  return SkColorGetA(color_) ? std::optional(color_) : std::nullopt;
}

void SidebarTint::OnPaint(gfx::Canvas* canvas) {
  if (!SkColorGetA(color_)) {
    return;
  }
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(color_);
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

void SidebarTint::AddedToWidget() {
  UpdateWindowTheme();
}

void SidebarTint::AnimationProgressed(const gfx::Animation* animation) {
  color_ = gfx::Tween::ColorValueBetween(
      gfx::Tween::CalculateValue(gfx::Tween::EASE_OUT,
                                 animation->GetCurrentValue()),
      from_, GetTargetColor());
  SchedulePaint();
}

void SidebarTint::OnViewBoundsChanged(views::View* observed_view) {
  SetBoundsRect(observed_view->GetLocalBounds());
}

void SidebarTint::OnViewIsDeleting(views::View* observed_view) {
  region_observation_.Reset();
}

void SidebarTint::OnSpaceSwitched() {
  UpdateColor(/*animate=*/true);
  UpdateWindowTheme();
}

// The finger moves the blend; letting go without switching fades back.
void SidebarTint::OnSpacePreviewChanged() {
  UpdateColor(/*animate=*/!features_->preview_space());
}

void SidebarTint::OnSpaceUpdated(const SpaceModel& space) {
  if (features_ && (&space == &features_->space_model() ||
                    &space == features_->preview_space())) {
    UpdateColor(/*animate=*/true);
  }
  if (features_ && &space == &features_->space_model()) {
    UpdateWindowTheme();
  }
}

SkColor SidebarTint::GetTargetColor() const {
  if (!features_) {
    return SK_ColorTRANSPARENT;
  }
  const SkColor color = ToTintColor(features_->space_model().color());
  SpaceModel* preview = features_->preview_space();
  return preview ? gfx::Tween::ColorValueBetween(
                       features_->preview_progress(), color,
                       ToTintColor(preview->color()))
                 : color;
}

void SidebarTint::UpdateColor(bool animate) {
  const SkColor target = GetTargetColor();
  if (animate && target != color_ &&
      gfx::Animation::ShouldRenderRichAnimation()) {
    from_ = color_;
    animation_.Stop();
    animation_.Start();
    return;
  }
  animation_.Stop();
  color_ = target;
  SchedulePaint();
}

void SidebarTint::UpdateWindowTheme() {
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser_);
  views::Widget* widget = browser_view ? browser_view->GetWidget() : nullptr;
  if (!widget || !features_) {
    return;
  }
  // Off the record keeps its grey; an extension theme paints its own colors.
  Profile* profile = browser_->GetProfile();
  const ThemeService* theme_service =
      ThemeServiceFactory::GetForProfile(profile);
  const bool keep_theme =
      profile->IsOffTheRecord() ||
      (theme_service && theme_service->UsingExtensionTheme());
  // Rebuilds the window's colors if the seed changed.
  widget->SetUserColorOverride(
      keep_theme ? std::nullopt
                 : GetSpaceTint(features_->space_model().color()));
}

BEGIN_METADATA(SidebarTint)
END_METADATA

}  // namespace strata
