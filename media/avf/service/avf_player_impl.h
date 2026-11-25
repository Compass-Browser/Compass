// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_SERVICE_AVF_PLAYER_IMPL_H_
#define COMPASS_MEDIA_AVF_SERVICE_AVF_PLAYER_IMPL_H_

#include <stdint.h>

#include <memory>
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
class AVFMseSourceImpl;

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

  // FairPlay for MSE: a player made with an empty `url` plays the HLS
  // sessions its AVFMseSourceImpl makes, one AVPlayerItem each.
  using MseItemCallback =
      base::OnceCallback<void(bool success, const std::string& error)>;
  void SetMseSource(std::unique_ptr<AVFMseSourceImpl> source);
  // Plays `url`, whose time 0 is media time `time_offset`; `ready` runs once
  // the item is ready to play or failed.
  void LoadMseItem(const GURL& url,
                   base::TimeDelta time_offset,
                   MseItemCallback ready);
  base::TimeDelta CurrentMediaTime() const;
  void ReportError(const std::string& message);

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
  // Diagnostics, with --vmodule=avf_player_impl=1.
  void OnAccessLogChanged();
  void OnErrorLogChanged();

 private:
  void Disconnect();
  void LogVariants();
  void ReplyToLoad(base::TimeDelta duration);
  void SendTimeUpdate();
  base::TimeDelta CurrentTime() const;
  double EffectiveRate() const;
  base::TimeDelta Duration() const;

  const bool mse_;
  // Media time of the item's 0 (MSE sessions start anywhere).
  base::TimeDelta time_offset_;
  MseItemCallback mse_ready_callback_;
  std::unique_ptr<AVFMseSourceImpl> mse_source_;

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
  // MSE: the session the current item's asset is a recipient of.
  AVContentKeySession* __strong mse_key_session_;
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
