// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/renderer/avf_renderer.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/base/cdm_context.h"
#include "media/base/pipeline_status.h"
#include "media/base/renderer_client.h"
#include "media/base/timestamp_constants.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"

namespace compass::avf {

AVFRenderer::AVFRenderer(media::VideoRendererSink* video_renderer_sink)
    : video_renderer_sink_(video_renderer_sink) {}

AVFRenderer::~AVFRenderer() {
  if (demuxer_) {
    // The pipeline destroys its renderer when it suspends (e.g. a paused or
    // background player); the AVPlayer lives on in the demuxer, so stop it.
    if (playing_ && demuxer_->player()) {
      demuxer_->player()->SetRate(0);
    }
    demuxer_->SetObserver(nullptr);
  }
}

void AVFRenderer::Initialize(media::MediaResource* media_resource,
                             media::RendererClient* client,
                             media::PipelineStatusCallback init_cb) {
  client_ = client;
  demuxer_ = AVFDemuxer::FromMediaResource(media_resource);
  if (!demuxer_ || !demuxer_->player()) {
    LOG(ERROR) << "AVFRenderer: not playing from an AVFDemuxer";
    std::move(init_cb).Run(media::PIPELINE_ERROR_INITIALIZATION_FAILED);
    return;
  }
  demuxer_->SetObserver(this);
  // After a pipeline suspend and resume this is a new renderer for an
  // AVPlayer that has been playing: start from its last state.
  have_enough_ = demuxer_->have_enough();
  {
    base::AutoLock lock(time_lock_);
    duration_ = demuxer_->duration();
    base_time_ = demuxer_->last_time();
  }
  if (!demuxer_->natural_size().IsEmpty()) {
    PaintHoleFrame(demuxer_->natural_size());
  }
  std::move(init_cb).Run(media::PIPELINE_OK);
}

void AVFRenderer::SetCdm(media::CdmContext* cdm_context,
                         CdmAttachedCB cdm_attached_cb) {
  // Only the FairPlay CDM (a MojoCdm for the CDM in the AVFoundation process,
  // //compass/media/avf/renderer/avf_cdm_factory.cc) has an id there.
  std::optional<base::UnguessableToken> cdm_id =
      cdm_context ? cdm_context->GetCdmId() : std::nullopt;
  if (!cdm_id || !demuxer_ || !demuxer_->player()) {
    std::move(cdm_attached_cb).Run(false);
    return;
  }
  demuxer_->player()->SetCdm(*cdm_id, std::move(cdm_attached_cb));
}

void AVFRenderer::SetLatencyHint(std::optional<base::TimeDelta> latency_hint) {
}

void AVFRenderer::Flush(base::OnceClosure flush_cb) {
  playing_ = false;
  if (demuxer_ && demuxer_->player()) {
    demuxer_->player()->SetRate(0);
  }
  base::TimeDelta now = GetMediaTime();
  {
    base::AutoLock lock(time_lock_);
    base_time_ = now;
    time_rate_ = 0;
  }
  // The pipeline waits for HAVE_ENOUGH again after the next
  // StartPlayingFrom(); make sure it's sent.
  buffering_state_ = media::BUFFERING_HAVE_NOTHING;
  std::move(flush_cb).Run();
}

void AVFRenderer::StartPlayingFrom(base::TimeDelta time) {
  playing_ = true;
  // A live stream can't be seeked; the pipeline still starts it "from 0",
  // which would send AVPlayer to the start of the live window. Stay where
  // AVPlayer is (near the live edge).
  if (demuxer_->is_live()) {
    {
      base::AutoLock lock(time_lock_);
      base_time_ = demuxer_->last_time();
      base_ticks_ = base::TimeTicks::Now();
      time_rate_ = 0;
    }
    seeking_ = false;
    OnSeekDone();
    return;
  }
  seeking_ = true;
  {
    base::AutoLock lock(time_lock_);
    base_time_ = time;
    base_ticks_ = base::TimeTicks::Now();
    time_rate_ = 0;
  }
  demuxer_->player()->Seek(
      time, base::BindOnce(&AVFRenderer::OnSeekDone,
                           weak_factory_.GetWeakPtr()));
}

void AVFRenderer::OnSeekDone() {
  seeking_ = false;
  if (!playing_) {
    return;
  }
  if (have_enough_) {
    SetBufferingState(media::BUFFERING_HAVE_ENOUGH);
  }
  demuxer_->player()->SetRate(playback_rate_);
}

void AVFRenderer::SetPlaybackRate(double playback_rate) {
  playback_rate_ = playback_rate;
  if (playing_ && !seeking_ && demuxer_ && demuxer_->player()) {
    demuxer_->player()->SetRate(playback_rate);
  }
}

void AVFRenderer::SetVolume(float volume) {
  if (demuxer_ && demuxer_->player()) {
    demuxer_->player()->SetVolume(volume);
  }
}

base::TimeDelta AVFRenderer::GetMediaTime() {
  base::AutoLock lock(time_lock_);
  base::TimeDelta time = base_time_;
  if (time_rate_ != 0) {
    time += (base::TimeTicks::Now() - base_ticks_) * time_rate_;
  }
  if (duration_ != media::kInfiniteDuration && duration_.is_positive()) {
    time = std::min(time, duration_);
  }
  return time;
}

media::RendererType AVFRenderer::GetRendererType() {
  return media::RendererType::kContentEmbedderDefined;
}

void AVFRenderer::OnTimeUpdate(base::TimeDelta media_time, double rate) {
  if (seeking_) {
    // From before the seek.
    return;
  }
  base::AutoLock lock(time_lock_);
  base_time_ = media_time;
  base_ticks_ = base::TimeTicks::Now();
  time_rate_ = playing_ ? rate : 0;
}

void AVFRenderer::OnBufferingStateChange(bool have_enough) {
  have_enough_ = have_enough;
  if (playing_ && !seeking_) {
    SetBufferingState(have_enough ? media::BUFFERING_HAVE_ENOUGH
                                  : media::BUFFERING_HAVE_NOTHING);
  }
}

void AVFRenderer::OnNaturalSizeChange(const gfx::Size& size) {
  PaintHoleFrame(size);
  client_->OnVideoNaturalSizeChange(size);
}

void AVFRenderer::OnEnded() {
  {
    base::AutoLock lock(time_lock_);
    if (duration_ != media::kInfiniteDuration && duration_.is_positive()) {
      base_time_ = duration_;
    }
    time_rate_ = 0;
  }
  client_->OnEnded();
}

void AVFRenderer::OnError(const std::string& message) {
  client_->OnError(media::PIPELINE_ERROR_DECODE);
}

void AVFRenderer::PaintHoleFrame(const gfx::Size& natural_size) {
  if (natural_size.IsEmpty()) {
    return;
  }
  video_renderer_sink_->PaintSingleFrame(media::VideoFrame::CreateVideoHoleFrame(
      demuxer_->overlay_plane_id(), natural_size, base::TimeDelta()));
}

void AVFRenderer::SetBufferingState(media::BufferingState state) {
  if (buffering_state_ == state) {
    return;
  }
  buffering_state_ = state;
  client_->OnBufferingStateChange(state,
                                  media::BUFFERING_CHANGE_REASON_UNKNOWN);
}

AVFRendererFactory::AVFRendererFactory() = default;
AVFRendererFactory::~AVFRendererFactory() = default;

std::unique_ptr<media::Renderer> AVFRendererFactory::CreateRenderer(
    const scoped_refptr<base::SequencedTaskRunner>& media_task_runner,
    const scoped_refptr<base::TaskRunner>& worker_task_runner,
    media::AudioRendererSink* audio_renderer_sink,
    media::VideoRendererSink* video_renderer_sink,
    media::RequestOverlayInfoCB request_overlay_info_cb,
    const gfx::ColorSpace& target_color_space) {
  return std::make_unique<AVFRenderer>(video_renderer_sink);
}

}  // namespace compass::avf
