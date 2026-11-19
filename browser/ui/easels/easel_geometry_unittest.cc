// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/easels/easel_geometry.h"

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::easels {
namespace {

PageView View(double device_pixel_ratio) {
  PageView view;
  view.viewport_width = 1000;
  view.viewport_height = 800;
  view.device_pixel_ratio = device_pixel_ratio;
  return view;
}

TEST(EaselGeometryTest, CssPixelsAreDipsWithoutZoom) {
  // A 2x screen: devicePixelRatio 2, the view's scale 2, no page zoom.
  EXPECT_EQ(gfx::Rect(10, 20, 300, 150),
            RegionToViewRect(gfx::RectF(10, 20, 300, 150), View(2), 2,
                             gfx::Size(1000, 800)));
}

TEST(EaselGeometryTest, PageZoomScalesTheRegion) {
  // 150% zoom on a 2x screen: devicePixelRatio 3.
  EXPECT_EQ(gfx::Rect(15, 30, 450, 225),
            RegionToViewRect(gfx::RectF(10, 20, 300, 150), View(3), 2,
                             gfx::Size(2000, 2000)));
  // 50% zoom on a 1x screen.
  EXPECT_EQ(gfx::Rect(5, 10, 150, 75),
            RegionToViewRect(gfx::RectF(10, 20, 300, 150), View(0.5), 1,
                             gfx::Size(2000, 2000)));
}

TEST(EaselGeometryTest, PinchZoomMovesAndScales) {
  PageView view = View(1);
  view.visual_left = 100;
  view.visual_top = 50;
  view.visual_scale = 2;
  EXPECT_EQ(gfx::Rect(20, 40, 200, 100),
            RegionToViewRect(gfx::RectF(110, 70, 100, 50), view, 1,
                             gfx::Size(1000, 800)));
}

TEST(EaselGeometryTest, CutToTheView) {
  EXPECT_EQ(gfx::Rect(900, 700, 100, 100),
            RegionToViewRect(gfx::RectF(900, 700, 400, 400), View(1), 1,
                             gfx::Size(1000, 800)));
  EXPECT_TRUE(RegionToViewRect(gfx::RectF(1200, 0, 50, 50), View(1), 1,
                               gfx::Size(1000, 800))
                  .IsEmpty());
}

TEST(EaselGeometryTest, RoundingErrorAddsNoRow) {
  // 1.1 zoom: 100 CSS px are 110.00000x DIPs in floats.
  EXPECT_EQ(gfx::Rect(11, 11, 110, 110),
            RegionToViewRect(gfx::RectF(10, 10, 100, 100), View(1.1), 1,
                             gfx::Size(1000, 800)));
}

TEST(EaselGeometryTest, BadNumbersFallBack) {
  PageView view = View(0);
  view.visual_scale = 0;
  // devicePixelRatio 0 is taken as the screen's: no zoom.
  EXPECT_EQ(gfx::Rect(10, 10, 20, 20),
            RegionToViewRect(gfx::RectF(10, 10, 20, 20), view, 2,
                             gfx::Size(100, 100)));
}

TEST(EaselGeometryTest, PixelSize) {
  EXPECT_EQ(gfx::Size(600, 300),
            ViewRectToPixelSize(gfx::Rect(10, 20, 300, 150), 2));
  EXPECT_EQ(gfx::Size(450, 225),
            ViewRectToPixelSize(gfx::Rect(0, 0, 300, 150), 1.5f));
}

TEST(EaselGeometryTest, ShiftForScroll) {
  // Saved at y 500, the page now stops at 300: the region is 200 lower.
  EXPECT_EQ(gfx::RectF(10, 220, 100, 50),
            ShiftForScroll(gfx::RectF(10, 20, 100, 50),
                           gfx::Vector2dF(0, 500), gfx::Vector2dF(0, 300)));
  EXPECT_EQ(gfx::RectF(10, 20, 100, 50),
            ShiftForScroll(gfx::RectF(10, 20, 100, 50),
                           gfx::Vector2dF(40, 500), gfx::Vector2dF(40, 500)));
}

TEST(EaselGeometryTest, PlacesRightOfEverything) {
  EXPECT_EQ(gfx::PointF(40, 40),
            PlaceNewObject({}));
  EXPECT_EQ(gfx::PointF(340, -20),
            PlaceNewObject({gfx::RectF(0, 0, 100, 100),
                            gfx::RectF(200, -20, 100, 30),
                            gfx::RectF(50, 400, 10, 10)}));
}

TEST(EaselGeometryTest, PageViewFromValue) {
  EXPECT_FALSE(PageViewFromValue(base::Value()).has_value());
  base::Value value(base::DictValue()
                        .Set("scrollX", 0)
                        .Set("scrollY", 1200.5)
                        .Set("innerWidth", 1280)
                        .Set("innerHeight", 720)
                        .Set("devicePixelRatio", 2)
                        .Set("visualLeft", 0)
                        .Set("visualTop", 0)
                        .Set("visualScale", 1));
  const std::optional<PageView> view = PageViewFromValue(value);
  ASSERT_TRUE(view.has_value());
  EXPECT_EQ(1200.5, view->scroll_y);
  EXPECT_EQ(1280, view->viewport_width);
  EXPECT_EQ(720, view->viewport_height);
  EXPECT_EQ(2, view->device_pixel_ratio);
  EXPECT_EQ(1, view->visual_scale);
}

}  // namespace
}  // namespace compass::easels
