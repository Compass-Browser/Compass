// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/avf_media_service.h"

#include <utility>

#include "base/functional/bind.h"
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
                     id));
  uint32_t context_id = impl->ca_context_id();
  players_[id] = std::move(impl);
  std::move(callback).Run(context_id);
}

void AVFMediaService::DeletePlayer(int id) {
  players_.erase(id);
}

std::unique_ptr<AVFMediaService> RunAVFMediaService(
    mojo::PendingReceiver<mojom::AVFMediaService> receiver) {
  return std::make_unique<AVFMediaService>(std::move(receiver));
}

}  // namespace compass::avf
