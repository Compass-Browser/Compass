// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_EASELS_EASEL_GEOMETRY_H_
#define COMPASS_BROWSER_UI_EASELS_EASEL_GEOMETRY_H_

#include <optional>
#include <vector>

#include "base/values.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/vector2d_f.h"

// The arithmetic of captures (docs/design/easels.md, "Captures"), apart from
// the browser so it can be tested on its own.
namespace compass::easels {

// A new object's distance from the board's edge, and from the objects
// already there.
inline constexpr double kPlacementMargin = 40;

// How the page was shown when a region of it was picked, as the page's
// script reports it (easel_page.js view()). CSS pixels.
struct PageView {
  double scroll_x = 0;
  double scroll_y = 0;
  // The layout viewport (innerWidth, innerHeight).
  double viewport_width = 0;
  double viewport_height = 0;
  // CSS pixels to physical ones: the screen's scale times the page's zoom.
  double device_pixel_ratio = 1;
  // The visual viewport within the layout viewport (pinch zoom).
  double visual_left = 0;
  double visual_top = 0;
  double visual_scale = 1;
};

// Rects and offsets from the doubles the sync messages and the page use.
gfx::RectF ToRectF(double x, double y, double width, double height);
gfx::Vector2dF ToVector2dF(double x, double y);

// view()'s dictionary; nullopt when it isn't one (the page went away).
std::optional<PageView> PageViewFromValue(const base::Value& value);

// A region of the page, in CSS pixels of its layout viewport, as a rect of
// the page's view in DIPs: what RenderWidgetHostView::CopyFromSurface()
// takes, and scales to device pixels itself. Cut to the view; empty when
// none of it shows.
gfx::Rect RegionToViewRect(const gfx::RectF& css_region,
                           const PageView& view,
                           float device_scale_factor,
                           const gfx::Size& view_size);

// The size in device pixels CopyFromSurface() returns for |view_rect|.
gfx::Size ViewRectToPixelSize(const gfx::Rect& view_rect,
                              float device_scale_factor);

// A region saved with the page scrolled to |saved_scroll|, in the viewport
// of the same page now scrolled to |scroll| (it may stop short of where it
// was: a shorter page, or a smaller window).
gfx::RectF ShiftForScroll(const gfx::RectF& region,
                          const gfx::Vector2dF& saved_scroll,
                          const gfx::Vector2dF& scroll);

// Where a new object's top left goes on a board whose objects take |boxes|:
// right of them all, level with the topmost, kPlacementMargin away; at
// (kPlacementMargin, kPlacementMargin) on an empty board. The editor's view
// of the board isn't known here, so it can't be its center.
gfx::PointF PlaceNewObject(const std::vector<gfx::RectF>& boxes);

}  // namespace compass::easels

#endif  // COMPASS_BROWSER_UI_EASELS_EASEL_GEOMETRY_H_
