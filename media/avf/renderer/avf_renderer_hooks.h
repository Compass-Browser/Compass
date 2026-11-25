// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_RENDERER_AVF_RENDERER_HOOKS_H_
#define COMPASS_MEDIA_AVF_RENDERER_AVF_RENDERER_HOOKS_H_

#include <memory>

#include "base/functional/function_ref.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"

class GURL;

namespace media {
class Demuxer;
class RendererFactory;
}  // namespace media

// Called by content/renderer/media/media_factory.cc
// (patches/content-renderer-media-media_factory.cc.patch) for each media
// player. Both return null unless `url` is played with AVFoundation.
namespace compass::avf {

// `bind_interface` binds a receiver through the frame's browser interface
// broker.
std::unique_ptr<media::Demuxer> MaybeCreateDemuxer(
    const GURL& url,
    base::FunctionRef<void(mojo::GenericPendingReceiver)> bind_interface);

std::unique_ptr<media::RendererFactory> MaybeCreateRendererFactory(
    const GURL& url);

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_RENDERER_AVF_RENDERER_HOOKS_H_
