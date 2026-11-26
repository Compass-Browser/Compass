// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/downloads_bubble/file_thumbnails.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "services/data_decoder/public/cpp/decode_image.h"
#include "services/data_decoder/public/mojom/image_decoder.mojom.h"
#include "skia/ext/image_operations.h"
#include "third_party/skia/include/core/SkBitmap.h"

namespace compass::downloads_bubble {

namespace {

// Bigger pictures get the file's icon: reading them would take too long.
constexpr int64_t kMaxPictureBytes = 30 * 1024 * 1024;

std::optional<std::vector<uint8_t>> ReadPicture(const base::FilePath& path) {
  std::optional<int64_t> size = base::GetFileSize(path);
  if (!size || *size <= 0 || *size > kMaxPictureBytes) {
    return std::nullopt;
  }
  return base::ReadFileToBytes(path);
}

// |bitmap| scaled down, keeping its shape, to cover |size|.
SkBitmap Shrink(const SkBitmap& bitmap, gfx::Size size) {
  if (bitmap.drawsNothing() || bitmap.width() <= size.width() ||
      bitmap.height() <= size.height()) {
    return bitmap;
  }
  const double scale =
      std::max(static_cast<double>(size.width()) / bitmap.width(),
               static_cast<double>(size.height()) / bitmap.height());
  return skia::ImageOperations::Resize(
      bitmap, skia::ImageOperations::RESIZE_GOOD,
      std::max(1, static_cast<int>(bitmap.width() * scale)),
      std::max(1, static_cast<int>(bitmap.height() * scale)));
}

}  // namespace

void LoadFilePreview(const base::FilePath& path,
                     downloads_folder::FileKind kind,
                     gfx::Size pixel_size,
                     base::OnceCallback<void(const SkBitmap&)> done) {
  if (kind != downloads_folder::FileKind::kImage) {
    std::move(done).Run(SkBitmap());
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadPicture, path),
      base::BindOnce(
          [](gfx::Size pixel_size,
             base::OnceCallback<void(const SkBitmap&)> done,
             std::optional<std::vector<uint8_t>> bytes) {
            if (!bytes) {
              std::move(done).Run(SkBitmap());
              return;
            }
            data_decoder::DecodeImageIsolated(
                *bytes, data_decoder::mojom::ImageCodec::kDefault,
                /*shrink_to_fit=*/true, data_decoder::kDefaultMaxSizeInBytes,
                gfx::Size(),
                base::BindOnce(
                    [](gfx::Size pixel_size,
                       base::OnceCallback<void(const SkBitmap&)> done,
                       const SkBitmap& bitmap) {
                      std::move(done).Run(Shrink(bitmap, pixel_size));
                    },
                    pixel_size, std::move(done)));
          },
          pixel_size, std::move(done)));
}

}  // namespace compass::downloads_bubble
