// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_RENDERER_AVF_MSE_RENDERER_H_
#define COMPASS_MEDIA_AVF_RENDERER_AVF_MSE_RENDERER_H_

#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "compass/media/avf/public/mojom/avf_player.mojom.h"
#include "media/base/buffering_state.h"
#include "media/base/demuxer_stream.h"
#include "media/base/renderer.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace media {
class VideoRendererSink;
}

namespace compass::avf {

// FairPlay for MSE (docs/design/fairplay.md): the media::Renderer for players
// whose CDM is FairPlay's, whatever their demuxer (MSE's ChunkDemuxer). It
// reads the demuxer streams and sends their samples, still encrypted, to the
// AVFoundation process, which plays them with AVPlayer from a local HLS copy;
// the picture is AVPlayer's layer through a video hole, as AVFRenderer's.
// Lives on the media thread.
class AVFMseRenderer : public media::Renderer, public mojom::AVFPlayerClient {
 public:
  AVFMseRenderer(media::VideoRendererSink* video_renderer_sink,
                 mojo::PendingRemote<mojom::AVFPlayerFactory> factory);
  AVFMseRenderer(const AVFMseRenderer&) = delete;
  AVFMseRenderer& operator=(const AVFMseRenderer&) = delete;
  ~AVFMseRenderer() override;

  // media::Renderer:
  void Initialize(media::MediaResource* media_resource,
                  media::RendererClient* client,
                  media::PipelineStatusCallback init_cb) override;
  void SetCdm(media::CdmContext* cdm_context,
              CdmAttachedCB cdm_attached_cb) override;
  void SetLatencyHint(std::optional<base::TimeDelta> latency_hint) override;
  void Flush(base::OnceClosure flush_cb) override;
  void StartPlayingFrom(base::TimeDelta time) override;
  void SetPlaybackRate(double playback_rate) override;
  void SetVolume(float volume) override;
  base::TimeDelta GetMediaTime() override;
  media::RendererType GetRendererType() override;

  // mojom::AVFPlayerClient:
  void OnTimeUpdate(base::TimeDelta media_time, double rate) override;
  void OnBufferingStateChange(bool have_enough) override;
  void OnBufferedRangesChange(
      std::vector<mojom::AVFTimeRangePtr> ranges) override;
  void OnNaturalSizeChange(const gfx::Size& size) override;
  void OnDurationChange(base::TimeDelta duration) override;
  void OnEnded() override;
  void OnError(const std::string& message) override;
  void OnEncrypted(const std::vector<uint8_t>& init_data) override;

 private:
  struct Reader {
    raw_ptr<media::DemuxerStream> stream = nullptr;
    mojom::AVFTrack track = mojom::AVFTrack::kVideo;
    bool reading = false;
    bool ended = false;
    base::TimeDelta sent_until;
  };

  void OnPlayerCreated(
      media::PipelineStatusCallback init_cb,
      const std::optional<base::UnguessableToken>& overlay_plane_id);
  void ReadMore(Reader& reader);
  void OnRead(Reader* reader,
              int session,
              media::DemuxerStream::Status status,
              media::DemuxerStream::DecoderBufferVector buffers);
  void OnSessionStarted(int session, bool success, const std::string& error);
  void OnPlayerGone();
  void PaintHoleFrame(const gfx::Size& natural_size);
  void SetBufferingState(media::BufferingState state);
  void Fail(media::PipelineStatusCodes status, const std::string& message);

  const raw_ptr<media::VideoRendererSink> video_renderer_sink_;
  mojo::PendingRemote<mojom::AVFPlayerFactory> pending_factory_;
  mojo::Remote<mojom::AVFPlayerFactory> factory_;
  mojo::Remote<mojom::AVFPlayer> player_;
  mojo::Remote<mojom::AVFMseSource> source_;
  mojo::Receiver<mojom::AVFPlayerClient> client_receiver_{this};
  raw_ptr<media::RendererClient> client_ = nullptr;

  std::optional<base::UnguessableToken> overlay_plane_id_;
  std::optional<base::UnguessableToken> cdm_id_;
  CdmAttachedCB pending_cdm_attached_cb_;
  Reader audio_;
  Reader video_;
  bool failed_ = false;

  // Each StartPlayingFrom() is a session; replies and reads of an earlier
  // one are dropped.
  int session_ = 0;
  double playback_rate_ = 0;
  bool playing_ = false;   // Between StartPlayingFrom() and Flush().
  bool starting_ = false;  // Until the session's AVPlayer item is ready.
  bool have_enough_ = false;
  media::BufferingState buffering_state_ = media::BUFFERING_HAVE_NOTHING;

  // GetMediaTime() may be called on any thread.
  mutable base::Lock time_lock_;
  base::TimeDelta base_time_ GUARDED_BY(time_lock_);
  base::TimeTicks base_ticks_ GUARDED_BY(time_lock_);
  double time_rate_ GUARDED_BY(time_lock_) = 0;

  base::WeakPtrFactory<AVFMseRenderer> weak_factory_{this};
};

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_RENDERER_AVF_MSE_RENDERER_H_
