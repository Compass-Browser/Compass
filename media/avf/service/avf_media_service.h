// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_SERVICE_AVF_MEDIA_SERVICE_H_
#define COMPASS_MEDIA_AVF_SERVICE_AVF_MEDIA_SERVICE_H_

#include <map>
#include <memory>

#include "compass/media/avf/public/mojom/avf_player.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace compass::avf {

class AVFPlayerImpl;

// The AVFoundation utility process's service: creates AVPlayers. Runs on the
// process's main thread, which has an NSApplication run loop
// (--message-loop-type-ui).
class AVFMediaService : public mojom::AVFMediaService {
 public:
  explicit AVFMediaService(
      mojo::PendingReceiver<mojom::AVFMediaService> receiver);
  AVFMediaService(const AVFMediaService&) = delete;
  AVFMediaService& operator=(const AVFMediaService&) = delete;
  ~AVFMediaService() override;

  // mojom::AVFMediaService:
  void CreatePlayer(const GURL& url,
                    mojom::AVFPlayerOptionsPtr options,
                    mojo::PendingReceiver<mojom::AVFPlayer> player,
                    mojo::PendingRemote<mojom::AVFPlayerClient> client,
                    CreatePlayerCallback callback) override;

 private:
  void DeletePlayer(int id);

  mojo::Receiver<mojom::AVFMediaService> receiver_;
  std::map<int, std::unique_ptr<AVFPlayerImpl>> players_;
  int next_id_ = 0;
};

// For chrome/utility/services.cc (patches/chrome-utility-services.cc.patch).
std::unique_ptr<AVFMediaService> RunAVFMediaService(
    mojo::PendingReceiver<mojom::AVFMediaService> receiver);

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_SERVICE_AVF_MEDIA_SERVICE_H_
