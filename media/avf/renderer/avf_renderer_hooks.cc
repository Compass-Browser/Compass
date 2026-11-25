// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/renderer/avf_renderer_hooks.h"

#include "compass/media/avf/common/avf_features.h"
#include "compass/media/avf/public/mojom/avf_player.mojom.h"
#include "compass/media/avf/renderer/avf_demuxer.h"
#include "compass/media/avf/renderer/avf_renderer.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "url/gurl.h"

namespace compass::avf {

std::unique_ptr<media::Demuxer> MaybeCreateDemuxer(
    const GURL& url,
    base::FunctionRef<void(mojo::GenericPendingReceiver)> bind_interface) {
  if (!ShouldUseAVFoundation(url)) {
    return nullptr;
  }
  mojo::PendingRemote<mojom::AVFPlayerFactory> factory;
  bind_interface(factory.InitWithNewPipeAndPassReceiver());
  return std::make_unique<AVFDemuxer>(url, std::move(factory));
}

std::unique_ptr<media::RendererFactory> MaybeCreateRendererFactory(
    const GURL& url) {
  if (!ShouldUseAVFoundation(url)) {
    return nullptr;
  }
  return std::make_unique<AVFRendererFactory>();
}

}  // namespace compass::avf
