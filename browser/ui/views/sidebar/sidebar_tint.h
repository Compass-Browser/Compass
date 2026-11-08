// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_TINT_H_
#define COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_TINT_H_

#include <array>
#include <optional>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/window_features.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkPath.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/color/color_provider_key.h"
#include "ui/gfx/animation/animation_delegate.h"
#include "ui/gfx/animation/linear_animation.h"
#include "ui/native_theme/native_theme.h"
#include "ui/native_theme/native_theme_observer.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"
#include "ui/views/widget/widget_observer.h"

class BrowserWindowInterface;

namespace compass {

// The color a space tints the sidebar with, or nullopt for none (grey).
std::optional<SkColor> GetSpaceTint(SpaceColor color);

// What a space paints over the frame color: its tint, partly transparent, or
// transparent for none.
SkColor GetSpaceFrameTint(SpaceColor color);

// Where a tint paints: the sidebar and the frame, or the empty pane, which
// Arc paints a little lighter.
enum class TintSurface { kFrame, kPane };

// The color the tint paints for a space color: duller (and on the frame a
// little darker), as Arc paints its spaces (docs/design/spaces.md,
// "Intensity and grain").
SkColor ToTintColor(SkColor color, TintSurface surface = TintSurface::kFrame);

// What a space's tint paints on |surface|: its gradient's stops, evenly
// spaced, as opaque as its intensity says (transparent for grey), and its
// grain's opacity.
struct SpaceTintLook {
  std::array<SkColor, kMaxSpaceGradientColors> stops{};
  float grain_opacity = 0;
};
SpaceTintLook GetSpaceTintLook(const SpaceModel& space, TintSurface surface);

// What the grain's opacity is multiplied by on a display of
// |device_scale_factor|. A grain is a physical pixel, and its opacity was
// measured on 2x displays (Arc on a Mac); at 1x (a 27" 2560x1440 at 100%) a
// pixel is twice as big and the same noise shows much more. 1 from 2x up,
// then in step with the scale, down to kMinGrainScaleMultiplier at 1x and
// below. The empty page has the same (empty.ts, grainScaleMultiplier()).
inline constexpr float kMinGrainScaleMultiplier = 0.5f;
float GrainScaleMultiplier(float device_scale_factor);

// The mode a space in automatic mode gives its windows: light or dark where
// its tint is strong enough to decide how light the frame is whatever the
// system's mode, so text stays readable over it; nullopt (the system's)
// otherwise.
std::optional<ui::ColorProviderKey::ColorMode> GetTintColorMode(
    const SpaceModel& space);

// The mode whose text reads best over |space|'s tint laid on |backdrop| (the
// frame color of the space's mode): kLight (dark text) where the blend
// contrasts more with black than with white, as WCAG measures contrast,
// kDark (light text) otherwise. The tint's colors are averaged.
ui::ColorProviderKey::ColorMode GetTintTextMode(const SpaceModel& space,
                                                SkColor backdrop);

// Tints the whole vertical tab strip region (the header, the sidebar page
// and the margins around it) with the window's space color, over the
// region's background and under its children. Ignored by the region's
// layout: it follows the region's bounds and the shape of its background.
// Another one, under the browser view's children, tints the window frame
// around the page (WindowFrame) over the frame's color, so the two match.
// A swipe in the sidebar blends in the color of the space it pulls in
// (WindowFeatures::SetSpacePreview()), and other changes fade.
//
// A space with a gradient paints it from the window's top left corner to its
// bottom right one, in both tints, so the frame goes on where the
// region leaves off. Fades and swipes blend the gradients stop by stop. See
// docs/design/spaces.md, "Gradients".
//
// It also themes the window: the space color is the window's theme seed
// (Widget::SetUserColorOverride()), so the frame, the page's margins, menus,
// bubbles and WebUI take colors from it, as from a profile's theme color.
// Not while a swipe is under way, and not for off-the-record windows or
// extension themes. The window's mode is the one its text reads best in over
// the tint (GetTintTextMode()); where that isn't the space's own mode, the
// tint first paints the frame color of the space's mode under itself. See docs/design/spaces.md, "Space colors", "Window
// theme" and "Swiping".
class SidebarTint : public views::View,
                    public gfx::AnimationDelegate,
                    public views::ViewObserver,
                    public views::WidgetObserver,
                    public WindowFeatures::Observer,
                    public SpaceList::Observer,
                    public ui::NativeThemeObserver {
  METADATA_HEADER(SidebarTint, views::View)

 public:
  explicit SidebarTint(BrowserWindowInterface* browser);
  SidebarTint(const SidebarTint&) = delete;
  SidebarTint& operator=(const SidebarTint&) = delete;
  ~SidebarTint() override;

  // Adds a tint to |host|, behind its other children: the region, or the
  // browser view for the window frame around the page (WindowFrame).
  static SidebarTint* Install(views::View* host,
                              BrowserWindowInterface* browser);

  // The color painted now, if any.
  // Fades from what |other| paints now to this one's color: the window that
  // takes |other|'s place in a switch to another profile's space goes on
  // where its tint was.
  void ContinueFrom(const SidebarTint& other);

  // Where the gradient starts, ends and is halfway, evenly spaced; all the
  // same for a plain color, transparent for none.
  using Stops = std::array<SkColor, kMaxSpaceGradientColors>;

  std::optional<SkColor> GetTintForTesting() const;
  const Stops& GetStopsForTesting() const { return look_.stops; }
  // The grain's strength now, 0 to 1.
  float GetGrainForTesting() const { return look_.grain; }
  bool IsAnimatingForTesting() const { return animation_.is_animating(); }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;
  void AddedToWidget() override;
  void RemovedFromWidget() override;

  // gfx::AnimationDelegate:
  void AnimationProgressed(const gfx::Animation* animation) override;

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewIsDeleting(views::View* observed_view) override;

  // views::WidgetObserver:
  void OnWidgetBoundsChanged(views::Widget* widget,
                             const gfx::Rect& new_bounds) override;

  // WindowFeatures::Observer:
  void OnSpaceSwitched() override;
  void OnSpacePreviewChanged() override;
  void OnSwappedAway() override;

  // SpaceList::Observer:
  void OnSpaceUpdated(const SpaceModel& space) override;

  // ui::NativeThemeObserver:
  void OnNativeThemeUpdated(ui::NativeTheme* observed_theme) override;

 private:
  // What a space paints: its stops (opacity from its intensity) and grain.
  struct Look {
    Stops stops{};
    float grain = 0;
    friend bool operator==(const Look&, const Look&) = default;
  };
  static Look ToLook(const SpaceModel& space);
  static Look Blend(double progress, const Look& from, const Look& to);

  // The window's space's look, blended toward the swiped-in space's.
  Look GetTargetLook() const;
  // Grain over the tint (docs/design/spaces.md, "Intensity and grain").
  void PaintGrain(gfx::Canvas* canvas, const SkPath* clip);
  // Fades from the color painted now to the target, or jumps there.
  void UpdateColor(bool animate);
  // Seeds the window's theme with its space's color, and gives the window
  // the mode its text reads best in over the tint (GetTintTextMode()).
  void UpdateWindowTheme();
  // Paints the frame color of the space's mode where the window's mode,
  // picked for its text, differs (UpdateWindowTheme()).
  void PaintBackdrop(gfx::Canvas* canvas, const SkPath* clip, float opacity);

  const raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<WindowFeatures> features_;
  // The window's colors in the space's mode, whose frame color lies under
  // the tint, when the window's mode differs.
  std::optional<ui::ColorProviderKey> backdrop_key_;
  // Painted now.
  Look look_;
  // Where the fade started.
  Look from_;
  // Once the window swaps away mid-slide, the look of the space it slides
  // to, which the page can no longer blend toward.
  std::optional<Look> swap_look_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};
  gfx::LinearAnimation animation_;
  base::CallbackListSubscription paint_as_active_subscription_;
  base::ScopedObservation<views::View, views::ViewObserver> region_observation_{
      this};
  base::ScopedObservation<WindowFeatures, WindowFeatures::Observer>
      features_observation_{this};
  base::ScopedObservation<SpaceList, SpaceList::Observer>
      space_list_observation_{this};
  // The system's mode, which a space in automatic mode follows.
  base::ScopedObservation<ui::NativeTheme, ui::NativeThemeObserver>
      native_theme_observation_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_TINT_H_
