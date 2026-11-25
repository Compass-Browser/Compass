// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_RENDERER_AVF_RENDERER_H_
#define COMPASS_MEDIA_AVF_RENDERER_AVF_RENDERER_H_

#include <memory>

#include "base/functional/callback.h"

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "compass/media/avf/renderer/avf_demuxer.h"
#include "media/base/buffering_state.h"
#include "media/base/renderer.h"
#include "media/base/renderer_factory.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"

namespace media {
class VideoRendererSink;
}

namespace compass::avf {

// The media::Renderer for AVFDemuxer: drives the AVPlayer in the AVFoundation
// utility process and shows its layer by painting a video-hole frame, which
// the GPU process replaces with the AVPlayer's CoreAnimation layer.
class AVFRenderer : public media::Renderer, public AVFPlaybackObserver {
 public:
  explicit AVFRenderer(media::VideoRendererSink* video_renderer_sink);
  AVFRenderer(const AVFRenderer&) = delete;
  AVFRenderer& operator=(const AVFRenderer&) = delete;
  ~AVFRenderer() override;

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

  // AVFPlaybackObserver:
  void OnTimeUpdate(base::TimeDelta media_time, double rate) override;
  void OnBufferingStateChange(bool have_enough) override;
  void OnNaturalSizeChange(const gfx::Size& size) override;
  void OnEnded() override;
  void OnError(const std::string& message) override;

 private:
  void PaintHoleFrame(const gfx::Size& natural_size);
  void OnSeekDone();
  void SetBufferingState(media::BufferingState state);

  const raw_ptr<media::VideoRendererSink> video_renderer_sink_;
  raw_ptr<media::RendererClient> client_ = nullptr;
  raw_ptr<AVFDemuxer> demuxer_ = nullptr;

  double playback_rate_ = 0;
  bool playing_ = false;   // Between StartPlayingFrom() and Flush().
  bool seeking_ = false;
  bool have_enough_ = false;
  media::BufferingState buffering_state_ = media::BUFFERING_HAVE_NOTHING;

  // GetMediaTime() may be called on any thread.
  mutable base::Lock time_lock_;
  base::TimeDelta base_time_ GUARDED_BY(time_lock_);
  base::TimeTicks base_ticks_ GUARDED_BY(time_lock_);
  double time_rate_ GUARDED_BY(time_lock_) = 0;
  base::TimeDelta duration_ GUARDED_BY(time_lock_);

  base::WeakPtrFactory<AVFRenderer> weak_factory_{this};
};

// AVFRenderer for native HLS, AVFMseRenderer for anything else (FairPlay for
// MSE).
class AVFRendererFactory : public media::RendererFactory {
 public:
  AVFRendererFactory(
      bool hls,
      base::RepeatingCallback<void(mojo::GenericPendingReceiver)>
          bind_interface);
  ~AVFRendererFactory() override;

  std::unique_ptr<media::Renderer> CreateRenderer(
      const scoped_refptr<base::SequencedTaskRunner>& media_task_runner,
      const scoped_refptr<base::TaskRunner>& worker_task_runner,
      media::AudioRendererSink* audio_renderer_sink,
      media::VideoRendererSink* video_renderer_sink,
      media::RequestOverlayInfoCB request_overlay_info_cb,
      const gfx::ColorSpace& target_color_space) override;

 private:
  const bool hls_;
  base::RepeatingCallback<void(mojo::GenericPendingReceiver)> bind_interface_;
};

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_RENDERER_AVF_RENDERER_H_
