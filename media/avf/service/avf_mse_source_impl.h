// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_SERVICE_AVF_MSE_SOURCE_IMPL_H_
#define COMPASS_MEDIA_AVF_SERVICE_AVF_MSE_SOURCE_IMPL_H_

#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "compass/media/avf/public/mojom/avf_player.mojom.h"
#include "compass/media/avf/service/mse/hls_session.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "url/gurl.h"

namespace compass::avf {

class AVFPlayerImpl;

// FairPlay for MSE (docs/design/fairplay.md): the renderer's demuxer streams
// become an HLS session (mse::HlsSession) that AVPlayer reads over loopback;
// each StartSession() (a pipeline seek) is a new session and AVPlayerItem.
// Owned by its AVFPlayerImpl, on the utility process's main thread.
class AVFMseSourceImpl : public mojom::AVFMseSource {
 public:
  AVFMseSourceImpl(mojo::PendingReceiver<mojom::AVFMseSource> receiver,
                   AVFPlayerImpl* player);
  AVFMseSourceImpl(const AVFMseSourceImpl&) = delete;
  AVFMseSourceImpl& operator=(const AVFMseSourceImpl&) = delete;
  ~AVFMseSourceImpl() override;

  // mojom::AVFMseSource:
  void StartSession(base::TimeDelta start,
                    const std::optional<media::AudioDecoderConfig>& audio,
                    const std::optional<media::VideoDecoderConfig>& video,
                    StartSessionCallback callback) override;
  void SetAudioConfig(const media::AudioDecoderConfig& config) override;
  void SetVideoConfig(const media::VideoDecoderConfig& config) override;
  void Append(mojom::AVFTrack track,
              std::vector<mojom::AVFSamplePtr> samples) override;
  void EndOfStream(mojom::AVFTrack track) override;

 private:
  // Loads the session into AVPlayer once it has enough, and reports errors.
  void Update();
  void OnItemReady(int session, bool success, const std::string& error);
  void OnSeeked(int session);
  void EndSession();

  mojo::Receiver<mojom::AVFMseSource> receiver_;
  const raw_ptr<AVFPlayerImpl> player_;

  scoped_refptr<mse::HlsSession> session_;
  int session_id_ = 0;
  GURL url_;
  base::TimeDelta start_;
  bool item_loading_ = false;
  bool error_reported_ = false;
  StartSessionCallback start_callback_;

  base::WeakPtrFactory<AVFMseSourceImpl> weak_factory_{this};
};

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_SERVICE_AVF_MSE_SOURCE_IMPL_H_
