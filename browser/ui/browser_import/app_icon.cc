// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/browser_import/app_icon.h"

#include <optional>
#include <vector>

#include "base/base64.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"

namespace compass::browser_import {

std::string PngDataUrl(const SkBitmap& bitmap) {
  if (bitmap.drawsNothing()) {
    return std::string();
  }
  const std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap,
                                        /*discard_transparency=*/false);
  if (!png) {
    return std::string();
  }
  return "data:image/png;base64," + base::Base64Encode(*png);
}

}  // namespace compass::browser_import
