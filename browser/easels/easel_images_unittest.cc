// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/easels/easel_images.h"

#include "compass/browser/easels/easel_store.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"

namespace compass::easels {
namespace {

SkBitmap Bitmap(int width, int height, SkColor color) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(width, height);
  bitmap.eraseColor(color);
  return bitmap;
}

TEST(EaselImagesTest, OpaqueBecomesJpeg) {
  auto image = FitBitmap(Bitmap(300, 200, SK_ColorBLUE));
  ASSERT_TRUE(image.has_value());
  EXPECT_EQ("image/jpeg", image->mime_type);
  EXPECT_EQ(300, image->width);
  EXPECT_EQ(200, image->height);
}

TEST(EaselImagesTest, TransparentStaysPng) {
  auto image = FitBitmap(Bitmap(40, 40, SK_ColorTRANSPARENT));
  ASSERT_TRUE(image.has_value());
  EXPECT_EQ("image/png", image->mime_type);
}

TEST(EaselImagesTest, LargeImagesAreScaledDown) {
  auto image = FitBitmap(Bitmap(5000, 1000, SK_ColorRED));
  ASSERT_TRUE(image.has_value());
  EXPECT_EQ(kMaxAssetPixels, image->width);
  EXPECT_EQ(409, image->height);
  EXPECT_LE(image->data.size(), kMaxAssetBytes);
}

TEST(EaselImagesTest, NoisyImagesFitTheByteLimit) {
  SkBitmap noise = Bitmap(2000, 2000, SK_ColorWHITE);
  uint32_t seed = 1;
  for (int y = 0; y < noise.height(); ++y) {
    for (int x = 0; x < noise.width(); ++x) {
      seed = seed * 1103515245 + 12345;
      *noise.getAddr32(x, y) = seed | 0xff000000;
    }
  }
  auto image = FitBitmap(noise);
  ASSERT_TRUE(image.has_value());
  EXPECT_LE(image->data.size(), kMaxAssetBytes);
}

TEST(EaselImagesTest, EmptyIsRefused) {
  EXPECT_FALSE(FitBitmap(SkBitmap()).has_value());
}

}  // namespace
}  // namespace compass::easels
