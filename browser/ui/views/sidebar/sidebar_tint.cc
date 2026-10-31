// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/sidebar/sidebar_tint.h"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/numerics/safe_conversions.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_image.h"
#include "cc/paint/paint_shader.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/custom_corners_background.h"
#include "chrome/browser/ui/views/frame/themed_background.h"
#include "compass/browser/window_sync/space_service.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkMatrix.h"
#include "third_party/skia/include/core/SkPath.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"

namespace compass {

namespace {

// How much of the space color shows over the frame color, at the default
// intensity: 14%.
constexpr SkAlpha kTintAlpha = 0x24;

// Grain at full strength: a tile of grey noise blended as an overlay (grey
// 128 leaves the color as is) at this opacity.
constexpr float kMaxGrainOpacity = 0.22f;
constexpr int kGrainTileSize = 128;

// The noise tile, the same every time (a fixed linear congruential
// sequence), in physical pixels.
const cc::PaintImage& GrainTile() {
  static const base::NoDestructor<cc::PaintImage> tile([] {
    SkBitmap bitmap;
    bitmap.allocN32Pixels(kGrainTileSize, kGrainTileSize, /*isOpaque=*/true);
    uint32_t state = 0x2545f491;
    for (int y = 0; y < kGrainTileSize; ++y) {
      for (int x = 0; x < kGrainTileSize; ++x) {
        state = state * 1664525u + 1013904223u;
        const uint8_t value = state >> 24;
        *bitmap.getAddr32(x, y) = SkPreMultiplyARGB(0xff, value, value, value);
      }
    }
    bitmap.setImmutable();
    return cc::PaintImage::CreateFromBitmap(std::move(bitmap));
  }());
  return *tile;
}

// The fade between two spaces' colors; the page's slide takes as long.
constexpr base::TimeDelta kFadeDuration = base::Milliseconds(280);

}  // namespace

SkColor GetSpaceFrameTint(SpaceColor color) {
  const std::optional<SkColor> tint = GetSpaceTint(color);
  return tint ? SkColorSetA(*tint, kTintAlpha) : SK_ColorTRANSPARENT;
}

std::optional<SkColor> GetSpaceTint(SpaceColor color) {
  return color.tint();
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
    look_ = GetTargetLook();
  }
}

SidebarTint::~SidebarTint() = default;

// static
SidebarTint* SidebarTint::Install(views::View* host,
                                  BrowserWindowInterface* browser) {
  auto* tint =
      host->AddChildViewAt(std::make_unique<SidebarTint>(browser), 0);
  tint->region_observation_.Observe(host);
  tint->SetBoundsRect(host->GetLocalBounds());
  return tint;
}

// static
// A space's color and gradient, sampled at the stops' evenly spaced
// positions, as opaque as its intensity says: the same three times for a
// plain color, its halfway blend in the middle for two colors.
SidebarTint::Look SidebarTint::ToLook(const SpaceModel& space) {
  if (space.color().is_grey()) {
    return Look();
  }
  const SkAlpha alpha =
      base::ClampRound<SkAlpha>(kMaxSpaceTintOpacity * space.intensity() * 255);
  std::vector<SkColor> colors = {SkColorSetA(*space.color().tint(), alpha)};
  for (SpaceColor color : space.gradient()) {
    colors.push_back(SkColorSetA(*color.tint(), alpha));
  }
  Look look;
  for (size_t i = 0; i < look.stops.size(); ++i) {
    const double at = static_cast<double>(i) / (look.stops.size() - 1) *
                      (colors.size() - 1);
    const size_t before = static_cast<size_t>(at);
    const size_t after = std::min(before + 1, colors.size() - 1);
    look.stops[i] = gfx::Tween::ColorValueBetween(at - before, colors[before],
                                                  colors[after]);
  }
  look.grain = space.grain();
  return look;
}

// static
SidebarTint::Look SidebarTint::Blend(double progress,
                                     const Look& from,
                                     const Look& to) {
  Look look;
  for (size_t i = 0; i < look.stops.size(); ++i) {
    look.stops[i] =
        gfx::Tween::ColorValueBetween(progress, from.stops[i], to.stops[i]);
  }
  look.grain = gfx::Tween::FloatValueBetween(progress, from.grain, to.grain);
  return look;
}

void SidebarTint::ContinueFrom(const SidebarTint& other) {
  look_ = other.look_;
  UpdateColor(/*animate=*/true);
}

std::optional<SkColor> SidebarTint::GetTintForTesting() const {
  return SkColorGetA(look_.stops[0]) ? std::optional(look_.stops[0])
                                     : std::nullopt;
}

void SidebarTint::OnPaint(gfx::Canvas* canvas) {
  // In the host's coordinates, which are ours: the region's background has
  // custom corners. The browser view has no background, so for the window
  // frame the tint first paints the frame color the region's background
  // paints, rather than leave the frame view's, which differs.
  const views::Background* background = parent()->background();
  const auto* corners =
      background ? background->AsA<CustomCornersBackground>() : nullptr;
  if (!corners) {
    if (BrowserView* browser_view =
            BrowserView::GetBrowserViewForBrowser(browser_)) {
      ThemedBackground::PaintBackground(canvas, this, browser_view,
                                        ThemedBackground::kFrameTheme);
    }
  }
  const Stops& stops = look_.stops;
  if (std::ranges::all_of(stops,
                          [](SkColor stop) { return !SkColorGetA(stop); })) {
    return;
  }
  // As opaque as the region's background: where it lets the frame show
  // through (the glass frame), the frame's tint shows too, and a second one
  // would double it.
  const float opacity = corners ? corners->primary_color().opacity : 1.f;
  std::array<SkColor4f, kMaxSpaceGradientColors> colors;
  for (size_t i = 0; i < colors.size(); ++i) {
    colors[i] = SkColor4f::FromColor(stops[i]);
    colors[i].fA *= opacity;
  }
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  if (std::ranges::all_of(stops,
                          [&](SkColor stop) { return stop == stops[0]; })) {
    flags.setColor(colors[0]);
  } else {
    // Across the whole window, from its top left corner to its bottom right
    // one, whichever view paints it.
    const gfx::Rect window = GetWidget()->GetRootView()->GetLocalBounds();
    gfx::Point start = window.origin();
    gfx::Point end = window.bottom_right();
    views::View::ConvertPointFromWidget(this, &start);
    views::View::ConvertPointFromWidget(this, &end);
    const std::array<SkPoint, 2> points = {gfx::PointToSkPoint(start),
                                           gfx::PointToSkPoint(end)};
    constexpr std::array<SkScalar, kMaxSpaceGradientColors> kPositions = {
        0.f, 0.5f, 1.f};
    // At 14%, the gradient spans a few levels of each channel over the
    // window, and each level would show as a band 50-100 DIP wide (Arc's
    // grain hides them); dithering mixes neighboring levels instead.
    flags.setDither(true);
    flags.setShader(cc::PaintShader::MakeLinearGradient(
        points.data(), colors.data(), kPositions.data(), colors.size(),
        SkTileMode::kClamp));
  }
  if (corners) {
    const SkPath path = corners->GetBackgroundPath();
    canvas->DrawPath(path, flags);
    PaintGrain(canvas, &path);
  } else {
    canvas->DrawRect(gfx::RectF(GetLocalBounds()), flags);
    PaintGrain(canvas, nullptr);
  }
}

void SidebarTint::PaintGrain(gfx::Canvas* canvas, const SkPath* clip) {
  if (look_.grain <= 0) {
    return;
  }
  // One grain per physical pixel, lined up across the window whichever view
  // paints it.
  gfx::Point origin;
  views::View::ConvertPointFromWidget(this, &origin);
  SkMatrix matrix = SkMatrix::Translate(origin.x(), origin.y());
  matrix.preScale(1 / canvas->image_scale(), 1 / canvas->image_scale());
  cc::PaintFlags flags;
  flags.setShader(cc::PaintShader::MakeImage(GrainTile(), SkTileMode::kRepeat,
                                             SkTileMode::kRepeat, &matrix));
  flags.setBlendMode(SkBlendMode::kOverlay);
  flags.setAlphaf(kMaxGrainOpacity * look_.grain);
  if (clip) {
    canvas->DrawPath(*clip, flags);
  } else {
    canvas->DrawRect(gfx::RectF(GetLocalBounds()), flags);
  }
}

void SidebarTint::AddedToWidget() {
  UpdateWindowTheme();
  // A gradient spans the window.
  widget_observation_.Reset();
  widget_observation_.Observe(GetWidget());
  // The frame color follows the window's activation.
  paint_as_active_subscription_ =
      GetWidget()->RegisterPaintAsActiveChangedCallback(base::BindRepeating(
          &SidebarTint::SchedulePaint, base::Unretained(this)));
}

void SidebarTint::RemovedFromWidget() {
  widget_observation_.Reset();
}

void SidebarTint::AnimationProgressed(const gfx::Animation* animation) {
  look_ = Blend(gfx::Tween::CalculateValue(gfx::Tween::EASE_OUT,
                                           animation->GetCurrentValue()),
                from_, GetTargetLook());
  SchedulePaint();
}

void SidebarTint::OnWidgetBoundsChanged(views::Widget* widget,
                                        const gfx::Rect& new_bounds) {
  if (look_.stops[0] != look_.stops[1] || look_.stops[1] != look_.stops[2] ||
      look_.grain > 0) {
    SchedulePaint();
  }
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

// The page's slide goes on, but its handler is gone: the tint finishes the
// blend itself, about as long as the slide has left.
void SidebarTint::OnSwappedAway() {
  if (SpaceModel* preview = features_->preview_space()) {
    swap_look_ = ToLook(*preview);
    UpdateColor(/*animate=*/true);
  }
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

SidebarTint::Look SidebarTint::GetTargetLook() const {
  if (!features_) {
    return Look();
  }
  if (swap_look_) {
    return *swap_look_;
  }
  const Look look = ToLook(features_->space_model());
  SpaceModel* preview = features_->preview_space();
  return preview ? Blend(features_->preview_progress(), look, ToLook(*preview))
                 : look;
}

void SidebarTint::UpdateColor(bool animate) {
  const Look target = GetTargetLook();
  if (animate && target != look_ &&
      gfx::Animation::ShouldRenderRichAnimation()) {
    from_ = look_;
    animation_.Stop();
    animation_.Start();
    return;
  }
  animation_.Stop();
  look_ = target;
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

}  // namespace compass
