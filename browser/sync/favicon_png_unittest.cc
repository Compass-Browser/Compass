// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/favicon_png.h"

#include <optional>
#include <string>
#include <vector>

#include "compass/browser/sync/compass_spaces_sync_bridge.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/codec/png_codec.h"

namespace compass::sync {
namespace {

std::vector<uint8_t> MakePng(int width, int height) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(width, height);
  bitmap.eraseColor(SK_ColorRED);
  return gfx::PNGCodec::EncodeBGRASkBitmap(bitmap,
                                           /*discard_transparency=*/false)
      .value();
}

TEST(SyncedFaviconPngTest, SmallIconsGoAsTheyAre) {
  const std::vector<uint8_t> png = MakePng(16, 16);
  std::optional<SyncedFaviconPng> result = MakeSyncedFaviconPng(png);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->source_size, 16);
  EXPECT_EQ(result->png, std::string(png.begin(), png.end()));
}

TEST(SyncedFaviconPngTest, LargeIconsAreScaledDown) {
  std::optional<SyncedFaviconPng> result =
      MakeSyncedFaviconPng(MakePng(192, 96));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->source_size, 192);
  EXPECT_LE(result->png.size(), kFaviconMaxBytes);
  const SkBitmap scaled = gfx::PNGCodec::Decode(base::as_byte_span(result->png));
  ASSERT_FALSE(scaled.isNull());
  EXPECT_EQ(scaled.width(), kFaviconMaxPixels);
  EXPECT_EQ(scaled.height(), kFaviconMaxPixels / 2);
}

TEST(SyncedFaviconPngTest, NotAPng) {
  const std::string junk = "not a png";
  EXPECT_FALSE(MakeSyncedFaviconPng(base::as_byte_span(junk)));
}

}  // namespace
}  // namespace compass::sync
