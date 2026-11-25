// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/renderer/avf_renderer_hooks.h"

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "mojo/public/cpp/bindings/remote.h"
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

namespace {

// Chrome's key systems, with FairPlay added once the browser said whether
// the page presents as Safari (which decides its init data types).
class KeySystemsWithFairPlay : public base::RefCounted<KeySystemsWithFairPlay> {
 public:
  explicit KeySystemsWithFairPlay(media::GetSupportedKeySystemsCB callback)
      : callback_(std::move(callback)) {}

  // After construction: binding `this` needs a reference held.
  void Start(
      base::FunctionRef<void(mojo::GenericPendingReceiver)> bind_interface) {
    bind_interface(factory_.BindNewPipeAndPassReceiver());
    factory_->IsPresentingAsSafari(
        base::BindOnce(&KeySystemsWithFairPlay::OnSafari, this));
    // A frame without the interface (none bound) still gets its key systems.
    factory_.set_disconnect_handler(
        base::BindOnce(&KeySystemsWithFairPlay::OnSafari, this, false));
  }

  void OnUpdate(media::KeySystemInfos key_systems) {
    if (!safari_) {
      pending_ = std::move(key_systems);
      return;
    }
    Run(std::move(key_systems));
  }

 private:
  friend class base::RefCounted<KeySystemsWithFairPlay>;
  ~KeySystemsWithFairPlay() = default;

  void OnSafari(bool safari) {
    if (safari_) {
      return;
    }
    safari_ = safari;
    factory_.reset();
    if (pending_) {
      Run(std::move(*pending_));
      pending_.reset();
    }
  }

  void Run(media::KeySystemInfos key_systems) {
    key_systems.push_back(std::make_unique<FairPlayKeySystemInfo>(*safari_));
    callback_.Run(std::move(key_systems));
  }

  media::GetSupportedKeySystemsCB callback_;
  mojo::Remote<mojom::AVFPlayerFactory> factory_;
  std::optional<bool> safari_;
  std::optional<media::KeySystemInfos> pending_;
};

}  // namespace

media::GetSupportedKeySystemsCB WrapKeySystemsCallback(
    media::GetSupportedKeySystemsCB callback,
    base::FunctionRef<void(mojo::GenericPendingReceiver)> bind_interface) {
  if (!IsFairPlayEnabled()) {
    return callback;
  }
  auto state = base::MakeRefCounted<KeySystemsWithFairPlay>(std::move(callback));
  state->Start(bind_interface);
  return base::BindRepeating(&KeySystemsWithFairPlay::OnUpdate, state);
}

}  // namespace compass::avf
