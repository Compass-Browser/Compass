// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/easels/easel_geometry.h"

#include <algorithm>
#include <cmath>

#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/geometry/size_f.h"

namespace compass::easels {

namespace {

// A finite number of |dict|, or |fallback|.
double Number(const base::DictValue& dict, const char* key, double fallback) {
  const std::optional<double> value = dict.FindDouble(key);
  return value && std::isfinite(*value) ? *value : fallback;
}

}  // namespace

gfx::RectF ToRectF(double x, double y, double width, double height) {
  return gfx::RectF(static_cast<float>(x), static_cast<float>(y),
                    static_cast<float>(width), static_cast<float>(height));
}

gfx::Vector2dF ToVector2dF(double x, double y) {
  return gfx::Vector2dF(static_cast<float>(x), static_cast<float>(y));
}

std::optional<PageView> PageViewFromValue(const base::Value& value) {
  const base::DictValue* dict = value.GetIfDict();
  if (!dict) {
    return std::nullopt;
  }
  PageView view;
  view.scroll_x = Number(*dict, "scrollX", 0);
  view.scroll_y = Number(*dict, "scrollY", 0);
  view.viewport_width = Number(*dict, "innerWidth", 0);
  view.viewport_height = Number(*dict, "innerHeight", 0);
  view.device_pixel_ratio = Number(*dict, "devicePixelRatio", 1);
  view.visual_left = Number(*dict, "visualLeft", 0);
  view.visual_top = Number(*dict, "visualTop", 0);
  view.visual_scale = Number(*dict, "visualScale", 1);
  return view;
}

gfx::Rect RegionToViewRect(const gfx::RectF& css_region,
                           const PageView& view,
                           float device_scale_factor,
                           const gfx::Size& view_size) {
  const double scale = device_scale_factor > 0 ? device_scale_factor : 1;
  // devicePixelRatio is the screen's scale times the page's zoom, without
  // pinch zoom: what's left over the screen's scale is the zoom, which a
  // CSS pixel is in DIPs.
  const double ratio =
      view.device_pixel_ratio > 0 ? view.device_pixel_ratio : scale;
  const double zoom = ratio / scale;
  const double pinch = view.visual_scale > 0 ? view.visual_scale : 1;
  // The view shows the visual viewport, which pinch zoom moves and scales
  // within the layout viewport the region is in.
  const double factor = pinch * zoom;
  const gfx::RectF dips(
      static_cast<float>((css_region.x() - view.visual_left) * factor),
      static_cast<float>((css_region.y() - view.visual_top) * factor),
      static_cast<float>(css_region.width() * factor),
      static_cast<float>(css_region.height() * factor));
  // Half a pixel's rounding error shouldn't add a row of the page around it.
  gfx::Rect rect = gfx::ToEnclosingRectIgnoringError(dips, 0.01f);
  rect.Intersect(gfx::Rect(view_size));
  return rect;
}

gfx::Size ViewRectToPixelSize(const gfx::Rect& view_rect,
                              float device_scale_factor) {
  const float scale = device_scale_factor > 0 ? device_scale_factor : 1;
  return gfx::ToRoundedSize(
      gfx::ScaleSize(gfx::SizeF(view_rect.size()), scale));
}

gfx::RectF ShiftForScroll(const gfx::RectF& region,
                          const gfx::Vector2dF& saved_scroll,
                          const gfx::Vector2dF& scroll) {
  gfx::RectF shifted = region;
  shifted.Offset(saved_scroll - scroll);
  return shifted;
}

gfx::PointF PlaceNewObject(const std::vector<gfx::RectF>& boxes) {
  if (boxes.empty()) {
    return gfx::PointF(static_cast<float>(kPlacementMargin),
                       static_cast<float>(kPlacementMargin));
  }
  float right = boxes.front().right();
  float top = boxes.front().y();
  for (const gfx::RectF& box : boxes) {
    right = std::max(right, box.right());
    top = std::min(top, box.y());
  }
  return gfx::PointF(right + static_cast<float>(kPlacementMargin), top);
}

}  // namespace compass::easels
