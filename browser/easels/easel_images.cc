// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/easels/easel_images.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/strings/grit/compass_strings.h"
#include "services/data_decoder/public/cpp/decode_image.h"
#include "services/data_decoder/public/mojom/image_decoder.mojom.h"
#include "skia/ext/image_operations.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/size.h"

namespace compass::easels {

namespace {

constexpr uint64_t kMaxInputBytes = 32 * 1024 * 1024;
constexpr int kJpegQuality = 85;

bool HasTransparency(const SkBitmap& bitmap) {
  if (bitmap.isOpaque()) {
    return false;
  }
  for (int y = 0; y < bitmap.height(); ++y) {
    for (int x = 0; x < bitmap.width(); ++x) {
      if (SkColorGetA(bitmap.getColor(x, y)) != 0xff) {
        return true;
      }
    }
  }
  return false;
}

SkBitmap ScaledToFit(const SkBitmap& bitmap, int max_side) {
  const int longest = std::max(bitmap.width(), bitmap.height());
  if (longest <= max_side) {
    return bitmap;
  }
  const double scale = static_cast<double>(max_side) / longest;
  const int width = std::max(1, static_cast<int>(bitmap.width() * scale));
  const int height = std::max(1, static_cast<int>(bitmap.height() * scale));
  return skia::ImageOperations::Resize(
      bitmap, skia::ImageOperations::RESIZE_GOOD, width, height);
}

std::optional<std::vector<uint8_t>> Encode(const SkBitmap& bitmap,
                                           bool png,
                                           int quality) {
  return png ? gfx::PNGCodec::EncodeBGRASkBitmap(bitmap,
                                                 /*discard_transparency=*/false)
             : gfx::JPEGCodec::Encode(bitmap, quality);
}

}  // namespace

base::expected<PreparedImage, std::string> FitBitmap(const SkBitmap& input) {
  if (input.drawsNothing() || input.width() <= 0 || input.height() <= 0) {
    return base::unexpected(
        l10n_util::GetStringUTF8(IDS_COMPASS_EASELS_IMAGE_UNREADABLE));
  }
  SkBitmap bitmap = input;
  if (bitmap.colorType() != kN32_SkColorType) {
    SkBitmap converted;
    if (!converted.tryAllocN32Pixels(bitmap.width(), bitmap.height()) ||
        !bitmap.readPixels(converted.pixmap())) {
      return base::unexpected(
          l10n_util::GetStringUTF8(IDS_COMPASS_EASELS_IMAGE_UNREADABLE));
    }
    bitmap = converted;
  }
  const bool png = HasTransparency(bitmap);
  int side = kMaxAssetPixels;
  int quality = kJpegQuality;
  // Smaller until it fits: a PNG gets smaller, a JPEG first loses quality.
  for (int attempt = 0; attempt < 12; ++attempt) {
    SkBitmap scaled = ScaledToFit(bitmap, side);
    std::optional<std::vector<uint8_t>> encoded = Encode(scaled, png, quality);
    if (!encoded) {
      return base::unexpected(
          l10n_util::GetStringUTF8(IDS_COMPASS_EASELS_IMAGE_NOT_ENCODED));
    }
    if (encoded->size() <= kMaxAssetBytes) {
      PreparedImage image;
      image.data.assign(encoded->begin(), encoded->end());
      image.mime_type = png ? "image/png" : "image/jpeg";
      image.width = scaled.width();
      image.height = scaled.height();
      return image;
    }
    if (!png && quality > 60) {
      quality -= 10;
    } else {
      side = std::max(64, side * 3 / 4);
    }
  }
  return base::unexpected(
      l10n_util::GetStringUTF8(IDS_COMPASS_EASELS_IMAGE_TOO_LARGE));
}

void PrepareBitmap(const SkBitmap& bitmap, PreparedImageCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&FitBitmap, bitmap), std::move(callback));
}

void PrepareImage(std::string encoded, PreparedImageCallback callback) {
  if (encoded.empty() || encoded.size() > kMaxInputBytes) {
    std::move(callback).Run(base::unexpected(
        l10n_util::GetStringUTF8(IDS_COMPASS_EASELS_IMAGE_TOO_LARGE)));
    return;
  }
  data_decoder::DecodeImageIsolated(
      base::as_byte_span(encoded), data_decoder::mojom::ImageCodec::kDefault,
      /*shrink_to_fit=*/true, kMaxInputBytes, gfx::Size(),
      base::BindOnce(
          [](PreparedImageCallback callback, const SkBitmap& bitmap) {
            if (bitmap.drawsNothing()) {
              std::move(callback).Run(base::unexpected(l10n_util::GetStringUTF8(
                  IDS_COMPASS_EASELS_IMAGE_UNREADABLE)));
              return;
            }
            PrepareBitmap(bitmap, std::move(callback));
          },
          std::move(callback)));
}

}  // namespace compass::easels
