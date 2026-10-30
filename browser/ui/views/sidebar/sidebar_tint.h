// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_TINT_H_
#define STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_TINT_H_

#include <optional>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/window_features.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/animation/animation_delegate.h"
#include "ui/gfx/animation/linear_animation.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"

class BrowserWindowInterface;

namespace strata {

// The color a space tints the sidebar with, or nullopt for none (grey). The
// sidebar page's palette (resources/sidebar/app.ts) shows the same colors.
std::optional<SkColor> GetSpaceTint(SpaceColor color);

// Tints the whole vertical tab strip region (the header, the sidebar page
// and the margins around it) with the window's space color, over the
// region's background and under its children. Ignored by the region's
// layout: it follows the region's bounds and the shape of its background.
// Another one, under the browser view's children, tints the window frame
// around the page (WindowFrame) over the frame's color, so the two match.
// A swipe in the sidebar blends in the color of the space it pulls in
// (WindowFeatures::SetSpacePreview()), and other changes fade.
//
// It also themes the window: the space color is the window's theme seed
// (Widget::SetUserColorOverride()), so the frame, the page's margins, menus,
// bubbles and WebUI take colors from it, as from a profile's theme color.
// Not while a swipe is under way, and not for off-the-record windows or
// extension themes. See docs/design/spaces.md, "Space colors", "Window
// theme" and "Swiping".
class SidebarTint : public views::View,
                    public gfx::AnimationDelegate,
                    public views::ViewObserver,
                    public WindowFeatures::Observer,
                    public SpaceList::Observer {
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

  std::optional<SkColor> GetTintForTesting() const;
  bool IsAnimatingForTesting() const { return animation_.is_animating(); }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;
  void AddedToWidget() override;

  // gfx::AnimationDelegate:
  void AnimationProgressed(const gfx::Animation* animation) override;

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewIsDeleting(views::View* observed_view) override;

  // WindowFeatures::Observer:
  void OnSpaceSwitched() override;
  void OnSpacePreviewChanged() override;
  void OnSwappedAway() override;

  // SpaceList::Observer:
  void OnSpaceUpdated(const SpaceModel& space) override;

 private:
  // The color for the window's space, blended toward the swiped-in space's;
  // transparent for none.
  SkColor GetTargetColor() const;
  // Fades from the color painted now to the target, or jumps there.
  void UpdateColor(bool animate);
  // Seeds the window's theme with its space's color.
  void UpdateWindowTheme();

  const raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<WindowFeatures> features_;
  // Painted now; transparent for none.
  SkColor color_ = SK_ColorTRANSPARENT;
  // Where the fade started.
  SkColor from_ = SK_ColorTRANSPARENT;
  // Once the window swaps away mid-slide, the color of the space it slides
  // to, which the page can no longer blend toward.
  std::optional<SkColor> swap_color_;
  gfx::LinearAnimation animation_;
  base::CallbackListSubscription paint_as_active_subscription_;
  base::ScopedObservation<views::View, views::ViewObserver> region_observation_{
      this};
  base::ScopedObservation<WindowFeatures, WindowFeatures::Observer>
      features_observation_{this};
  base::ScopedObservation<SpaceList, SpaceList::Observer>
      space_list_observation_{this};
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_TINT_H_
