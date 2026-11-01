// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/favicon_png.h"

#include <algorithm>
#include <vector>

#include "compass/browser/sync/compass_spaces_sync_bridge.h"
#include "skia/ext/image_operations.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"

namespace compass::sync {

std::optional<SyncedFaviconPng> MakeSyncedFaviconPng(
    base::span<const uint8_t> source) {
  const SkBitmap bitmap = gfx::PNGCodec::Decode(source);
  if (bitmap.isNull() || bitmap.width() <= 0 || bitmap.height() <= 0) {
    return std::nullopt;
  }
  SyncedFaviconPng result;
  result.source_size = std::max(bitmap.width(), bitmap.height());
  if (result.source_size <= kFaviconMaxPixels &&
      source.size() <= kFaviconMaxBytes) {
    // As stored: the same icon gives the same bytes on every pass.
    result.png.assign(source.begin(), source.end());
    return result;
  }
  for (int side : {64, 48, 32}) {
    if (side > result.source_size) {
      continue;
    }
    // The larger side becomes |side|, keeping the aspect ratio.
    const int width = std::max(
        1, bitmap.width() * side / result.source_size);
    const int height = std::max(
        1, bitmap.height() * side / result.source_size);
    const SkBitmap scaled = skia::ImageOperations::Resize(
        bitmap, skia::ImageOperations::RESIZE_BEST, width, height);
    std::optional<std::vector<uint8_t>> png =
        gfx::PNGCodec::EncodeBGRASkBitmap(scaled,
                                          /*discard_transparency=*/false);
    if (png && !png->empty() && png->size() <= kFaviconMaxBytes) {
      result.png.assign(png->begin(), png->end());
      return result;
    }
  }
  return std::nullopt;
}

}  // namespace compass::sync
