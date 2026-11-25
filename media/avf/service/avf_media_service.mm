// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/avf_media_service.h"

#include <utility>

#include "base/functional/bind.h"
#include "compass/media/avf/service/avf_cdm.h"
#include "compass/media/avf/service/avf_mse_source_impl.h"
#include "compass/media/avf/service/avf_player_impl.h"

namespace compass::avf {

AVFMediaService::AVFMediaService(
    mojo::PendingReceiver<mojom::AVFMediaService> receiver)
    : receiver_(this, std::move(receiver)) {}

AVFMediaService::~AVFMediaService() = default;

void AVFMediaService::CreatePlayer(
    const GURL& url,
    mojom::AVFPlayerOptionsPtr options,
    mojo::PendingReceiver<mojom::AVFPlayer> player,
    mojo::PendingRemote<mojom::AVFPlayerClient> client,
    CreatePlayerCallback callback) {
  if (!url.SchemeIsHTTPOrHTTPS()) {
    std::move(callback).Run(0);
    return;
  }
  int id = next_id_++;
  auto impl = std::make_unique<AVFPlayerImpl>(
      url, std::move(options), std::move(player), std::move(client),
      base::BindOnce(&AVFMediaService::DeletePlayer, base::Unretained(this),
                     id),
      base::BindRepeating(&AVFMediaService::FindCdm, base::Unretained(this)));
  uint32_t context_id = impl->ca_context_id();
  players_[id] = std::move(impl);
  std::move(callback).Run(context_id);
}

void AVFMediaService::CreateMsePlayer(
    mojo::PendingReceiver<mojom::AVFPlayer> player,
    mojo::PendingReceiver<mojom::AVFMseSource> source,
    mojo::PendingRemote<mojom::AVFPlayerClient> client,
    CreateMsePlayerCallback callback) {
  int id = next_id_++;
  // An empty URL: the player plays what `source` makes.
  auto impl = std::make_unique<AVFPlayerImpl>(
      GURL(), mojom::AVFPlayerOptions::New(), std::move(player),
      std::move(client),
      base::BindOnce(&AVFMediaService::DeletePlayer, base::Unretained(this),
                     id),
      base::BindRepeating(&AVFMediaService::FindCdm, base::Unretained(this)));
  impl->SetMseSource(
      std::make_unique<AVFMseSourceImpl>(std::move(source), impl.get()));
  uint32_t context_id = impl->ca_context_id();
  players_[id] = std::move(impl);
  std::move(callback).Run(context_id);
}

void AVFMediaService::DeletePlayer(int id) {
  players_.erase(id);
}

void AVFMediaService::CreateCdm(
    mojo::PendingReceiver<media::mojom::ContentDecryptionModule> cdm,
    CreateCdmCallback callback) {
  base::UnguessableToken id = base::UnguessableToken::Create();
  // The CDM goes when the renderer's MojoCdm does.
  cdms_[id] = std::make_unique<AVFCdm>(
      id, std::move(cdm),
      base::BindOnce(&AVFMediaService::DeleteCdm, base::Unretained(this), id));
  std::move(callback).Run(id);
}

void AVFMediaService::DeleteCdm(const base::UnguessableToken& id) {
  cdms_.erase(id);
}

AVFCdm* AVFMediaService::FindCdm(const base::UnguessableToken& id) {
  auto it = cdms_.find(id);
  return it == cdms_.end() ? nullptr : it->second.get();
}

std::unique_ptr<AVFMediaService> RunAVFMediaService(
    mojo::PendingReceiver<mojom::AVFMediaService> receiver) {
  return std::make_unique<AVFMediaService>(std::move(receiver));
}

}  // namespace compass::avf
