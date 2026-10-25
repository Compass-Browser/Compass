// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_TINT_H_
#define STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_TINT_H_

#include <optional>

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/window_features.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"
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
// See docs/design/spaces.md, "Space colors".
class SidebarTint : public views::View,
                    public views::ViewObserver,
                    public WindowFeatures::Observer,
                    public SpaceList::Observer {
  METADATA_HEADER(SidebarTint, views::View)

 public:
  explicit SidebarTint(BrowserWindowInterface* browser);
  SidebarTint(const SidebarTint&) = delete;
  SidebarTint& operator=(const SidebarTint&) = delete;
  ~SidebarTint() override;

  // Adds a tint to |region|, behind its other children.
  static SidebarTint* Install(views::View* region,
                              BrowserWindowInterface* browser);

  // The color painted now, if any.
  std::optional<SkColor> GetTintForTesting() const;

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewIsDeleting(views::View* observed_view) override;

  // WindowFeatures::Observer:
  void OnSpaceSwitched() override;

  // SpaceList::Observer:
  void OnSpaceUpdated(const SpaceModel& space) override;

 private:
  raw_ptr<WindowFeatures> features_;
  base::ScopedObservation<views::View, views::ViewObserver> region_observation_{
      this};
  base::ScopedObservation<WindowFeatures, WindowFeatures::Observer>
      features_observation_{this};
  base::ScopedObservation<SpaceList, SpaceList::Observer>
      space_list_observation_{this};
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_TINT_H_
