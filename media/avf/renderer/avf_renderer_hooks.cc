// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/renderer/avf_renderer_hooks.h"

#include "base/functional/bind.h"
#include "compass/media/avf/common/avf_features.h"
#include "compass/media/avf/renderer/avf_cdm_factory.h"
#include "compass/media/avf/renderer/fairplay_key_system_info.h"
#include "compass/media/avf/public/mojom/avf_player.mojom.h"
#include "compass/media/avf/renderer/avf_demuxer.h"
#include "compass/media/avf/renderer/avf_renderer.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "build/build_config.h"
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

std::unique_ptr<media::CdmFactory> WrapCdmFactory(
    std::unique_ptr<media::CdmFactory> factory,
    base::RepeatingCallback<void(mojo::GenericPendingReceiver)>
        bind_interface) {
#if BUILDFLAG(IS_MAC)
  return std::make_unique<AVFCdmFactory>(std::move(factory),
                                         std::move(bind_interface));
#else
  return factory;
#endif
}

media::GetSupportedKeySystemsCB WrapKeySystemsCallback(
    media::GetSupportedKeySystemsCB callback) {
  return base::BindRepeating(
      [](const media::GetSupportedKeySystemsCB& callback,
         media::KeySystemInfos key_systems) {
        if (IsFairPlayEnabled()) {
          key_systems.push_back(std::make_unique<FairPlayKeySystemInfo>());
        }
        callback.Run(std::move(key_systems));
      },
      std::move(callback));
}

}  // namespace compass::avf
