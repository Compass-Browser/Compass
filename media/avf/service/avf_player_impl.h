// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_SERVICE_AVF_PLAYER_IMPL_H_
#define COMPASS_MEDIA_AVF_SERVICE_AVF_PLAYER_IMPL_H_

#include <stdint.h>

#include <optional>
#include <set>
#include <string>

#include "base/functional/callback.h"
#include "base/unguessable_token.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "compass/media/avf/public/mojom/avf_player.mojom.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"

#if defined(__OBJC__)
@class AVContentKeyRequest;
@class AVContentKeySession;
@class CompassAVFKeyDelegate;
@class NSError;
@class NSMutableArray;
@class AVPlayer;
@class AVPlayerItem;
@class AVPlayerLayer;
@class CompassAVFObserver;
@class CAContext;
#else
class AVPlayer;
class AVPlayerItem;
class AVPlayerLayer;
class CompassAVFObserver;
class CAContext;
#endif

namespace compass::avf {

class AVFCdm;

// One AVPlayer, in the AVFoundation utility process, on its main thread.
// Its AVPlayerLayer is the root of a CAContext the GPU process hosts.
class AVFPlayerImpl : public mojom::AVFPlayer {
 public:
  using FindCdmCallback =
      base::RepeatingCallback<AVFCdm*(const base::UnguessableToken&)>;

  AVFPlayerImpl(const GURL& url,
                mojom::AVFPlayerOptionsPtr options,
                mojo::PendingReceiver<mojom::AVFPlayer> receiver,
                mojo::PendingRemote<mojom::AVFPlayerClient> client,
                base::OnceClosure on_disconnect,
                FindCdmCallback find_cdm);
  AVFPlayerImpl(const AVFPlayerImpl&) = delete;
  AVFPlayerImpl& operator=(const AVFPlayerImpl&) = delete;
  ~AVFPlayerImpl() override;

  uint32_t ca_context_id() const;

  // mojom::AVFPlayer:
  void Load(LoadCallback callback) override;
  void SetRate(double rate) override;
  void Seek(base::TimeDelta time, SeekCallback callback) override;
  void SetVolume(float volume) override;
  void SetCdm(const base::UnguessableToken& cdm_id,
              SetCdmCallback callback) override;

#if defined(__OBJC__)
  // From the player's AVContentKeySession (FairPlay).
  void OnKeyRequest(AVContentKeyRequest* request, bool renewal);
  void OnKeyRequestSucceeded(AVContentKeyRequest* request);
  void OnKeyRequestFailed(AVContentKeyRequest* request, NSError* error);
#endif

  // From CompassAVFObserver.
  void OnItemStatusChanged();
  void OnAssetDurationLoaded();
  void OnPresentationSizeChanged();
  void OnLoadedRangesChanged();
  void OnDurationChanged();
  void OnPlaybackStateChanged();
  void OnPlayedToEnd();
  void OnFailedToPlayToEnd(const std::string& message);
  void OnPeriodicTime();

 private:
  void Disconnect();
  void ReplyToLoad(base::TimeDelta duration);
  void SendTimeUpdate();
  base::TimeDelta CurrentTime() const;
  double EffectiveRate() const;
  base::TimeDelta Duration() const;

  mojo::Receiver<mojom::AVFPlayer> receiver_;
  mojo::Remote<mojom::AVFPlayerClient> client_;
  base::OnceClosure on_disconnect_;

  AVPlayer* __strong player_;
  AVPlayerItem* __strong item_;
  AVPlayerLayer* __strong layer_;
  CAContext* __strong context_;
  CompassAVFObserver* __strong observer_;

  // FairPlay: the asset's key requests go to this session; the page's CDM
  // (SetCdm()) answers them.
  AVContentKeySession* __strong key_session_;
  CompassAVFKeyDelegate* __strong key_delegate_;
  NSMutableArray* __strong key_requests_;
  // skd:// URIs the page got an "encrypted" event for (once each, as in
  // Safari; AVFoundation's retries go to the session that has the key).
  std::set<std::string> announced_keys_;
  FindCdmCallback find_cdm_;
  base::WeakPtr<AVFCdm> cdm_;

  LoadCallback load_callback_;
  // The rate the page asked for; AVPlayer only plays once the page does.
  double requested_rate_ = 0;
  bool ended_ = false;
  std::optional<bool> have_enough_;
  gfx::Size natural_size_;
  base::TimeDelta last_sent_time_;
  double last_sent_rate_ = -1;

  base::WeakPtrFactory<AVFPlayerImpl> weak_factory_{this};
};

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_SERVICE_AVF_PLAYER_IMPL_H_
