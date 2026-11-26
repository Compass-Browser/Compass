// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_DOWNLOADS_BUBBLE_FILE_THUMBNAILS_H_
#define COMPASS_BROWSER_UI_VIEWS_DOWNLOADS_BUBBLE_FILE_THUMBNAILS_H_

#include <map>
#include <tuple>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/time/time.h"
#include "compass/browser/downloads_folder/folder_files.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image_skia.h"

class SkBitmap;

namespace compass::downloads_bubble {

// Previews of the bubble's files. macOS: QuickLook's (pictures, videos,
// PDFs, documents; else the file's icon). Elsewhere: pictures decoded out of
// process, else the file's icon. Kept for the bubble's life, by path, size
// and change time, so a refresh of the list doesn't load them again.
class FileThumbnails {
 public:
  using Callback = base::OnceCallback<void(const gfx::ImageSkia&)>;

  FileThumbnails();
  FileThumbnails(const FileThumbnails&) = delete;
  FileThumbnails& operator=(const FileThumbnails&) = delete;
  ~FileThumbnails();

  // |size| in DIPs; made for 2x screens. |done| runs only if it loads, and
  // never after this is gone.
  void Load(const downloads_folder::FolderFile& file,
            gfx::Size size,
            Callback done);

 private:
  using Key = std::tuple<base::FilePath, int, int, base::Time>;

  void OnPreview(Key key, Callback done, const SkBitmap& bitmap);
  void LoadIcon(Key key, Callback done);

  std::map<Key, gfx::ImageSkia> cache_;
  base::CancelableTaskTracker icon_tracker_;
  base::WeakPtrFactory<FileThumbnails> weak_factory_{this};
};

// The platform's preview of |path| at |pixel_size| (at most); an empty
// bitmap if there's none. |done| runs on the calling sequence.
void LoadFilePreview(const base::FilePath& path,
                     downloads_folder::FileKind kind,
                     gfx::Size pixel_size,
                     base::OnceCallback<void(const SkBitmap&)> done);

}  // namespace compass::downloads_bubble

#endif  // COMPASS_BROWSER_UI_VIEWS_DOWNLOADS_BUBBLE_FILE_THUMBNAILS_H_
