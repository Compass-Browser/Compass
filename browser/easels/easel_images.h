// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_EASELS_EASEL_IMAGES_H_
#define COMPASS_BROWSER_EASELS_EASEL_IMAGES_H_

#include <string>

#include "base/functional/callback.h"
#include "base/types/expected.h"

class SkBitmap;

namespace compass::easels {

// An image ready for EaselStore::AddAsset(): at most kMaxAssetPixels a side
// and kMaxAssetBytes, PNG when it has transparency, else JPEG.
struct PreparedImage {
  std::string data;
  std::string mime_type;
  int width = 0;
  int height = 0;
};

using PreparedImageCallback =
    base::OnceCallback<void(base::expected<PreparedImage, std::string>)>;

// Decodes |encoded| (PNG, JPEG, WebP, GIF's first frame, ...) in the data
// decoder service, never in this process, then fits it to the limits off
// the UI thread. Input over 32 MB is refused.
void PrepareImage(std::string encoded, PreparedImageCallback callback);

// The same for a bitmap already decoded (a capture of a page).
void PrepareBitmap(const SkBitmap& bitmap, PreparedImageCallback callback);

// What PrepareBitmap() does, synchronously, for tests and the thread pool.
base::expected<PreparedImage, std::string> FitBitmap(const SkBitmap& bitmap);

}  // namespace compass::easels

#endif  // COMPASS_BROWSER_EASELS_EASEL_IMAGES_H_
