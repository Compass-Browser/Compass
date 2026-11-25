// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/renderer/avf_mse_renderer.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "media/base/cdm_context.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decrypt_config.h"
#include "media/base/media_resource.h"
#include "media/base/pipeline_status.h"
#include "media/base/renderer_client.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace compass::avf {
namespace {

// How far ahead of AVPlayer the samples go; MSE players keep about 30 s
// buffered.
constexpr base::TimeDelta kReadAhead = base::Seconds(20);
constexpr uint32_t kReadBatch = 32;

mojom::AVFSamplePtr ToSample(const media::DecoderBuffer& buffer) {
  auto sample = mojom::AVFSample::New();
  sample->data = mojo_base::BigBuffer(buffer);
  sample->timestamp = buffer.timestamp();
  sample->duration = buffer.duration();
  sample->is_key_frame = buffer.is_key_frame();
  if (const media::DecryptConfig* config = buffer.decrypt_config()) {
    auto encryption = mojom::AVFSampleEncryption::New();
    encryption->cbcs =
        config->encryption_scheme() == media::EncryptionScheme::kCbcs;
    encryption->key_id.assign(config->key_id().begin(),
                              config->key_id().end());
    encryption->iv.assign(config->iv().begin(), config->iv().end());
    for (const auto& subsample : config->subsamples()) {
      encryption->subsamples.push_back(mojom::AVFSubsample::New(
          subsample.clear_bytes, subsample.cypher_bytes));
    }
    if (config->encryption_pattern()) {
      encryption->has_pattern = true;
      encryption->crypt_byte_block =
          config->encryption_pattern()->crypt_byte_block();
      encryption->skip_byte_block =
          config->encryption_pattern()->skip_byte_block();
    }
    sample->encryption = std::move(encryption);
  }
  return sample;
}

}  // namespace

AVFMseRenderer::AVFMseRenderer(
    media::VideoRendererSink* video_renderer_sink,
    mojo::PendingRemote<mojom::AVFPlayerFactory> factory)
    : video_renderer_sink_(video_renderer_sink),
      pending_factory_(std::move(factory)) {
  audio_.track = mojom::AVFTrack::kAudio;
  video_.track = mojom::AVFTrack::kVideo;
}

AVFMseRenderer::~AVFMseRenderer() {
  if (pending_cdm_attached_cb_) {
    std::move(pending_cdm_attached_cb_).Run(false);
  }
}

void AVFMseRenderer::Initialize(media::MediaResource* media_resource,
                                media::RendererClient* client,
                                media::PipelineStatusCallback init_cb) {
  VLOG(1) << "FairPlay MSE: renderer initializing";
  client_ = client;
  for (media::DemuxerStream* stream : media_resource->GetAllStreams()) {
    if (stream->type() == media::DemuxerStream::AUDIO && !audio_.stream) {
      audio_.stream = stream;
    } else if (stream->type() == media::DemuxerStream::VIDEO &&
               !video_.stream) {
      video_.stream = stream;
    }
  }
  if (!audio_.stream && !video_.stream) {
    std::move(init_cb).Run(media::PIPELINE_ERROR_INITIALIZATION_FAILED);
    return;
  }
  factory_.Bind(std::move(pending_factory_));
  factory_->CreateMsePlayer(
      player_.BindNewPipeAndPassReceiver(),
      source_.BindNewPipeAndPassReceiver(),
      client_receiver_.BindNewPipeAndPassRemote(),
      base::BindOnce(&AVFMseRenderer::OnPlayerCreated,
                     weak_factory_.GetWeakPtr(), std::move(init_cb)));
  // The AVFoundation process crashed, or the player failed.
  player_.set_disconnect_handler(base::BindOnce(
      &AVFMseRenderer::OnPlayerGone, weak_factory_.GetWeakPtr()));
  if (cdm_id_) {
    player_->SetCdm(*cdm_id_, pending_cdm_attached_cb_
                                  ? std::move(pending_cdm_attached_cb_)
                                  : base::DoNothing());
  }
}

void AVFMseRenderer::OnPlayerCreated(
    media::PipelineStatusCallback init_cb,
    const std::optional<base::UnguessableToken>& overlay_plane_id) {
  if (!overlay_plane_id) {
    LOG(ERROR) << "FairPlay MSE: no AVFoundation player";
    std::move(init_cb).Run(media::PIPELINE_ERROR_INITIALIZATION_FAILED);
    return;
  }
  overlay_plane_id_ = overlay_plane_id;
  if (video_.stream) {
    gfx::Size size = video_.stream->video_decoder_config().natural_size();
    PaintHoleFrame(size);
    client_->OnVideoNaturalSizeChange(size);
  }
  std::move(init_cb).Run(media::PIPELINE_OK);
}

void AVFMseRenderer::SetCdm(media::CdmContext* cdm_context,
                            CdmAttachedCB cdm_attached_cb) {
  std::optional<base::UnguessableToken> cdm_id =
      cdm_context ? cdm_context->GetCdmId() : std::nullopt;
  if (!cdm_id) {
    std::move(cdm_attached_cb).Run(false);
    return;
  }
  cdm_id_ = cdm_id;
  if (player_.is_bound()) {
    player_->SetCdm(*cdm_id, std::move(cdm_attached_cb));
  } else {
    // Before Initialize() (the pipeline sets the CDM first).
    if (pending_cdm_attached_cb_) {
      std::move(pending_cdm_attached_cb_).Run(false);
    }
    pending_cdm_attached_cb_ = std::move(cdm_attached_cb);
  }
}

void AVFMseRenderer::SetLatencyHint(
    std::optional<base::TimeDelta> latency_hint) {}

void AVFMseRenderer::Flush(base::OnceClosure flush_cb) {
  playing_ = false;
  starting_ = false;
  ++session_;
  if (player_.is_bound()) {
    player_->SetRate(0);
  }
  base::TimeDelta now = GetMediaTime();
  {
    base::AutoLock lock(time_lock_);
    base_time_ = now;
    time_rate_ = 0;
  }
  buffering_state_ = media::BUFFERING_HAVE_NOTHING;
  std::move(flush_cb).Run();
}

void AVFMseRenderer::StartPlayingFrom(base::TimeDelta time) {
  VLOG(1) << "FairPlay MSE: start from " << time.InSecondsF() << "s, audio "
          << !!audio_.stream << " video " << !!video_.stream;
  playing_ = true;
  starting_ = true;
  have_enough_ = false;
  ++session_;
  {
    base::AutoLock lock(time_lock_);
    base_time_ = time;
    base_ticks_ = base::TimeTicks::Now();
    time_rate_ = 0;
  }
  std::optional<media::AudioDecoderConfig> audio_config;
  std::optional<media::VideoDecoderConfig> video_config;
  if (audio_.stream) {
    audio_config = audio_.stream->audio_decoder_config();
  }
  if (video_.stream) {
    video_config = video_.stream->video_decoder_config();
  }
  source_->StartSession(time, audio_config, video_config,
                        base::BindOnce(&AVFMseRenderer::OnSessionStarted,
                                       weak_factory_.GetWeakPtr(), session_));
  for (Reader* reader : {&audio_, &video_}) {
    reader->ended = false;
    reader->sent_until = time;
    ReadMore(*reader);
  }
}

void AVFMseRenderer::ReadMore(Reader& reader) {
  if (!reader.stream || !playing_ || reader.reading || reader.ended ||
      failed_) {
    return;
  }
  // Starting needs samples; later, stay a little ahead of AVPlayer.
  if (!starting_ && reader.sent_until - GetMediaTime() > kReadAhead) {
    return;
  }
  reader.reading = true;
  reader.stream->Read(
      kReadBatch, base::BindOnce(&AVFMseRenderer::OnRead,
                                 weak_factory_.GetWeakPtr(), &reader, session_));
}

void AVFMseRenderer::OnRead(Reader* reader,
                            int session,
                            media::DemuxerStream::Status status,
                            media::DemuxerStream::DecoderBufferVector buffers) {
  reader->reading = false;
  if (session != session_) {
    // From before a seek (usually kAborted). StartPlayingFrom() couldn't read
    // while this was pending; read for the new session now.
    ReadMore(*reader);
    return;
  }
  switch (status) {
    case media::DemuxerStream::kAborted:
      return;
    case media::DemuxerStream::kError:
      Fail(media::PIPELINE_ERROR_READ, "Demuxer read failed");
      return;
    case media::DemuxerStream::kConfigChanged:
      if (reader->track == mojom::AVFTrack::kAudio) {
        source_->SetAudioConfig(reader->stream->audio_decoder_config());
      } else {
        source_->SetVideoConfig(reader->stream->video_decoder_config());
      }
      ReadMore(*reader);
      return;
    case media::DemuxerStream::kOk:
      break;
  }
  VLOG(2) << "FairPlay MSE: read " << buffers.size() << " "
          << (reader->track == mojom::AVFTrack::kVideo ? "video" : "audio")
          << (buffers.empty() || buffers.back()->end_of_stream()
                  ? ""
                  : " to " + base::NumberToString(
                                 buffers.back()->timestamp().InSecondsF()));
  std::vector<mojom::AVFSamplePtr> samples;
  bool ended = false;
  for (const auto& buffer : buffers) {
    if (buffer->end_of_stream()) {
      ended = true;
      break;
    }
    reader->sent_until =
        std::max(reader->sent_until, buffer->timestamp() + buffer->duration());
    samples.push_back(ToSample(*buffer));
  }
  if (!samples.empty()) {
    source_->Append(reader->track, std::move(samples));
  }
  if (ended) {
    reader->ended = true;
    source_->EndOfStream(reader->track);
    return;
  }
  ReadMore(*reader);
}

void AVFMseRenderer::OnSessionStarted(int session,
                                      bool success,
                                      const std::string& error) {
  if (session != session_) {
    return;
  }
  VLOG(1) << "FairPlay MSE: session started: " << success << " " << error;
  if (!success) {
    Fail(media::PIPELINE_ERROR_DECODE, error);
    return;
  }
  starting_ = false;
  {
    base::AutoLock lock(time_lock_);
    base_ticks_ = base::TimeTicks::Now();
  }
  if (have_enough_) {
    SetBufferingState(media::BUFFERING_HAVE_ENOUGH);
  }
  player_->SetRate(playback_rate_);
}

void AVFMseRenderer::SetPlaybackRate(double playback_rate) {
  playback_rate_ = playback_rate;
  if (playing_ && !starting_ && player_.is_bound()) {
    player_->SetRate(playback_rate);
  }
}

void AVFMseRenderer::SetVolume(float volume) {
  if (player_.is_bound()) {
    player_->SetVolume(volume);
  }
}

base::TimeDelta AVFMseRenderer::GetMediaTime() {
  base::AutoLock lock(time_lock_);
  base::TimeDelta time = base_time_;
  if (time_rate_ != 0) {
    time += (base::TimeTicks::Now() - base_ticks_) * time_rate_;
  }
  return time;
}

media::RendererType AVFMseRenderer::GetRendererType() {
  return media::RendererType::kContentEmbedderDefined;
}

void AVFMseRenderer::OnTimeUpdate(base::TimeDelta media_time, double rate) {
  if (starting_ || !playing_) {
    // The previous session's, or AVPlayer seeking to the start.
    return;
  }
  {
    base::AutoLock lock(time_lock_);
    base_time_ = media_time;
    base_ticks_ = base::TimeTicks::Now();
    time_rate_ = rate;
  }
  ReadMore(audio_);
  ReadMore(video_);
}

void AVFMseRenderer::OnBufferingStateChange(bool have_enough) {
  have_enough_ = have_enough;
  if (playing_ && !starting_) {
    SetBufferingState(have_enough ? media::BUFFERING_HAVE_ENOUGH
                                  : media::BUFFERING_HAVE_NOTHING);
  }
}

void AVFMseRenderer::OnBufferedRangesChange(
    std::vector<mojom::AVFTimeRangePtr> ranges) {
  // MSE's buffered ranges are the SourceBuffers', not AVPlayer's.
}

void AVFMseRenderer::OnNaturalSizeChange(const gfx::Size& size) {
  PaintHoleFrame(size);
  client_->OnVideoNaturalSizeChange(size);
}

void AVFMseRenderer::OnDurationChange(base::TimeDelta duration) {
  // The MediaSource has the duration.
}

void AVFMseRenderer::OnEnded() {
  {
    base::AutoLock lock(time_lock_);
    time_rate_ = 0;
  }
  client_->OnEnded();
}

void AVFMseRenderer::OnError(const std::string& message) {
  Fail(media::PIPELINE_ERROR_DECODE, message);
}

void AVFMseRenderer::OnEncrypted(const std::vector<uint8_t>& init_data) {
  // The page's demuxer fired "encrypted" from the stream's init segment.
}

void AVFMseRenderer::OnPlayerGone() {
  Fail(media::PIPELINE_ERROR_DECODE, "The AVFoundation process went away");
}

void AVFMseRenderer::Fail(media::PipelineStatusCodes status,
                          const std::string& message) {
  if (failed_) {
    return;
  }
  failed_ = true;
  LOG(ERROR) << "FairPlay MSE: " << message;
  if (client_) {
    client_->OnError(status);
  }
}

void AVFMseRenderer::PaintHoleFrame(const gfx::Size& natural_size) {
  if (natural_size.IsEmpty() || !overlay_plane_id_) {
    return;
  }
  video_renderer_sink_->PaintSingleFrame(media::VideoFrame::CreateVideoHoleFrame(
      *overlay_plane_id_, natural_size, base::TimeDelta()));
}

void AVFMseRenderer::SetBufferingState(media::BufferingState state) {
  if (buffering_state_ == state) {
    return;
  }
  buffering_state_ = state;
  client_->OnBufferingStateChange(state,
                                  media::BUFFERING_CHANGE_REASON_UNKNOWN);
}

}  // namespace compass::avf
