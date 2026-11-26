// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/downloads_bubble/file_thumbnails.h"

#import <Foundation/Foundation.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>

#include <utility>

#include "base/apple/foundation_util.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "skia/ext/skia_utils_mac.h"
#include "third_party/skia/include/core/SkBitmap.h"

namespace compass::downloads_bubble {

void LoadFilePreview(const base::FilePath& path,
                     downloads_folder::FileKind kind,
                     gfx::Size pixel_size,
                     base::OnceCallback<void(const SkBitmap&)> done) {
  // QuickLook draws what Finder shows for any file: a picture's or video's
  // frame, a PDF's first page, else the file's icon.
  __block base::OnceCallback<void(const SkBitmap&)> reply =
      base::BindPostTask(base::SequencedTaskRunner::GetCurrentDefault(),
                         std::move(done));
  QLThumbnailGenerationRequest* request = [[QLThumbnailGenerationRequest alloc]
        initWithFileAtURL:base::apple::FilePathToNSURL(path)
                     size:CGSizeMake(pixel_size.width() / 2.0,
                                     pixel_size.height() / 2.0)
                    scale:2.0
      representationTypes:QLThumbnailGenerationRequestRepresentationTypeAll];
  [QLThumbnailGenerator.sharedGenerator
      generateBestRepresentationForRequest:request
                         completionHandler:^(
                             QLThumbnailRepresentation* thumbnail,
                             NSError* error) {
                           SkBitmap bitmap;
                           if (thumbnail && thumbnail.CGImage) {
                             bitmap =
                                 skia::CGImageToSkBitmap(thumbnail.CGImage);
                           }
                           std::move(reply).Run(bitmap);
                         }];
}

}  // namespace compass::downloads_bubble
