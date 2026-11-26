// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/downloads_bubble/file_thumbnails.h"

#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/icon_loader.h"
#include "chrome/browser/icon_manager.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_skia_rep.h"

namespace compass::downloads_bubble {

namespace {

// Thumbnails are made for 2x screens; 1x ones scale them down.
constexpr float kScale = 2.0f;

}  // namespace

FileThumbnails::FileThumbnails() = default;
FileThumbnails::~FileThumbnails() = default;

void FileThumbnails::Load(const downloads_folder::FolderFile& file,
                          gfx::Size size,
                          Callback done) {
  Key key{file.path, size.width(), size.height(), file.added};
  if (auto it = cache_.find(key); it != cache_.end()) {
    std::move(done).Run(it->second);
    return;
  }
  const gfx::Size pixel_size(size.width() * kScale, size.height() * kScale);
  LoadFilePreview(file.path, file.is_directory ? downloads_folder::FileKind::kOther : file.kind,
                  pixel_size,
                  base::BindOnce(&FileThumbnails::OnPreview,
                                 weak_factory_.GetWeakPtr(), std::move(key),
                                 std::move(done)));
}

void FileThumbnails::OnPreview(Key key, Callback done, const SkBitmap& bitmap) {
  if (bitmap.drawsNothing()) {
    LoadIcon(std::move(key), std::move(done));
    return;
  }
  gfx::ImageSkia image =
      gfx::ImageSkia::CreateFromBitmap(bitmap, kScale);
  cache_[key] = image;
  std::move(done).Run(image);
}

void FileThumbnails::LoadIcon(Key key, Callback done) {
  IconManager* icons = g_browser_process->icon_manager();
  if (!icons) {
    return;
  }
  icons->LoadIcon(
      std::get<0>(key), IconLoader::LARGE, kScale,
      base::BindOnce(
          [](base::WeakPtr<FileThumbnails> self, Key key, Callback done,
             gfx::Image icon) {
            if (!self || icon.IsEmpty()) {
              return;
            }
            gfx::ImageSkia image = icon.AsImageSkia();
            self->cache_[key] = image;
            std::move(done).Run(image);
          },
          weak_factory_.GetWeakPtr(), std::move(key), std::move(done)),
      &icon_tracker_);
}

}  // namespace compass::downloads_bubble
