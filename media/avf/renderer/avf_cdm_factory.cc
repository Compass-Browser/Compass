// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/renderer/avf_cdm_factory.h"

#include <utility>

#include "base/functional/bind.h"
#include "compass/media/avf/common/avf_features.h"
#include "compass/media/avf/public/mojom/avf_player.mojom.h"
#include "compass/media/avf/renderer/fairplay_key_system_info.h"
#include "media/base/cdm_config.h"
#include "media/mojo/clients/mojo_cdm.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace compass::avf {
namespace {

void OnCdmCreated(
    std::unique_ptr<mojo::Remote<mojom::AVFPlayerFactory>> factory,
    const media::CdmConfig& cdm_config,
    const media::SessionMessageCB& session_message_cb,
    const media::SessionClosedCB& session_closed_cb,
    const media::SessionKeysChangeCB& session_keys_change_cb,
    const media::SessionExpirationUpdateCB& session_expiration_update_cb,
    media::CdmCreatedCB cdm_created_cb,
    mojo::PendingRemote<media::mojom::ContentDecryptionModule> cdm,
    media::mojom::CdmContextPtr cdm_context) {
  if (!cdm || !cdm_context) {
    std::move(cdm_created_cb)
        .Run(nullptr, media::CreateCdmStatus::kUnknownError);
    return;
  }
  std::move(cdm_created_cb)
      .Run(base::MakeRefCounted<media::MojoCdm>(
               mojo::Remote<media::mojom::ContentDecryptionModule>(
                   std::move(cdm)),
               std::move(cdm_context), cdm_config, session_message_cb,
               session_closed_cb, session_keys_change_cb,
               session_expiration_update_cb),
           media::CreateCdmStatus::kSuccess);
}

}  // namespace

AVFCdmFactory::AVFCdmFactory(std::unique_ptr<media::CdmFactory> other,
                             BindInterfaceCallback bind_interface)
    : other_(std::move(other)), bind_interface_(std::move(bind_interface)) {}

AVFCdmFactory::~AVFCdmFactory() = default;

void AVFCdmFactory::Create(
    const media::CdmConfig& cdm_config,
    const media::SessionMessageCB& session_message_cb,
    const media::SessionClosedCB& session_closed_cb,
    const media::SessionKeysChangeCB& session_keys_change_cb,
    const media::SessionExpirationUpdateCB& session_expiration_update_cb,
    media::CdmCreatedCB cdm_created_cb) {
  if (cdm_config.key_system != kFairPlayKeySystem || !IsFairPlayEnabled()) {
    other_->Create(cdm_config, session_message_cb, session_closed_cb,
                   session_keys_change_cb, session_expiration_update_cb,
                   std::move(cdm_created_cb));
    return;
  }
  auto factory = std::make_unique<mojo::Remote<mojom::AVFPlayerFactory>>();
  bind_interface_.Run(factory->BindNewPipeAndPassReceiver());
  mojom::AVFPlayerFactory* factory_ptr = factory->get();
  // The callback keeps the factory's pipe alive until the reply.
  factory_ptr->CreateCdm(base::BindOnce(
      &OnCdmCreated, std::move(factory), cdm_config, session_message_cb,
      session_closed_cb, session_keys_change_cb, session_expiration_update_cb,
      std::move(cdm_created_cb)));
}

}  // namespace compass::avf
