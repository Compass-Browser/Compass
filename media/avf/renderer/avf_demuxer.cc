// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/renderer/avf_demuxer.h"

#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "media/base/audio_decoder_config.h"
#include "media/base/channel_layout.h"
#include "media/base/decoder_buffer.h"
#include "media/base/encryption_scheme.h"
#include "media/base/ranges.h"
#include "media/base/sample_format.h"
#include "media/base/timestamp_constants.h"
#include "media/base/video_decoder_config.h"
#include "media/base/video_transformation.h"

namespace compass::avf {
namespace {

// Live AVFDemuxers, so that AVFRenderer can tell its MediaResource is one.
struct LiveDemuxers {
  base::Lock lock;
  std::set<const AVFDemuxer*> set;
};

LiveDemuxers& GetLiveDemuxers() {
  static base::NoDestructor<LiveDemuxers> live;
  return *live;
}

}  // namespace

// A stream that is never read: it only tells the pipeline (and through it
// WebMediaPlayerImpl) that there is audio or video, and the video's size.
class AVFDemuxer::PlaceholderStream : public media::DemuxerStream {
 public:
  explicit PlaceholderStream(Type type, const gfx::Size& natural_size)
      : type_(type), natural_size_(natural_size) {}

  // media::DemuxerStream:
  void Read(uint32_t count, ReadCB read_cb) override {
    // AVFRenderer never reads; anything else that does gets end of stream.
    std::move(read_cb).Run(
        kOk, {media::DecoderBuffer::CreateEOSBuffer()});
  }
  media::AudioDecoderConfig audio_decoder_config() override {
    return media::AudioDecoderConfig(
        media::AudioCodec::kAAC, media::kSampleFormatF32,
        media::ChannelLayoutConfig::Stereo(), 48000, {},
        media::EncryptionScheme::kUnencrypted);
  }
  media::VideoDecoderConfig video_decoder_config() override {
    gfx::Size size = natural_size_.IsEmpty() ? gfx::Size(16, 9) : natural_size_;
    return media::VideoDecoderConfig(
        media::VideoCodec::kH264, media::H264PROFILE_MAIN,
        media::VideoDecoderConfig::AlphaMode::kIsOpaque,
        media::VideoColorSpace::REC709(), media::kNoTransformation, size,
        gfx::Rect(size), size, {}, media::EncryptionScheme::kUnencrypted);
  }
  Type type() const override { return type_; }
  media::StreamLiveness liveness() const override {
    return media::StreamLiveness::kRecorded;
  }
  bool SupportsConfigChanges() override { return false; }

 private:
  const Type type_;
  const gfx::Size natural_size_;
};

AVFDemuxer::AVFDemuxer(const GURL& url,
                       mojo::PendingRemote<mojom::AVFPlayerFactory> factory)
    : url_(url), pending_factory_(std::move(factory)) {
  LiveDemuxers& live = GetLiveDemuxers();
  base::AutoLock lock(live.lock);
  live.set.insert(this);
}

AVFDemuxer::~AVFDemuxer() {
  LiveDemuxers& live = GetLiveDemuxers();
  base::AutoLock lock(live.lock);
  live.set.erase(this);
}

// static
AVFDemuxer* AVFDemuxer::FromMediaResource(media::MediaResource* resource) {
  LiveDemuxers& live = GetLiveDemuxers();
  base::AutoLock lock(live.lock);
  for (const AVFDemuxer* demuxer : live.set) {
    if (static_cast<const media::MediaResource*>(demuxer) == resource) {
      return const_cast<AVFDemuxer*>(demuxer);
    }
  }
  return nullptr;
}

void AVFDemuxer::SetObserver(AVFPlaybackObserver* observer) {
  observer_ = observer;
}

std::vector<raw_ptr<media::DemuxerStream>> AVFDemuxer::GetAllStreams() {
  std::vector<raw_ptr<media::DemuxerStream>> streams;
  for (auto& stream : streams_) {
    streams.push_back(stream.get());
  }
  return streams;
}

std::string AVFDemuxer::GetDisplayName() const {
  return "AVFDemuxer";
}

media::DemuxerType AVFDemuxer::GetDemuxerType() const {
  // No type of its own; DemuxerManager deletes a kStreamProviderDemuxer
  // override on the media thread, which this needs (its pipes live there).
  return media::DemuxerType::kStreamProviderDemuxer;
}

void AVFDemuxer::Initialize(media::DemuxerHost* host,
                            media::PipelineStatusCallback status_cb) {
  host_ = host;
  init_cb_ = std::move(status_cb);
  factory_.Bind(std::move(pending_factory_));
  factory_.set_disconnect_handler(base::BindOnce(
      &AVFDemuxer::OnDisconnected, weak_factory_.GetWeakPtr()));
  factory_->CreatePlayer(
      url_, player_.BindNewPipeAndPassReceiver(),
      client_receiver_.BindNewPipeAndPassRemote(),
      base::BindOnce(&AVFDemuxer::OnPlayerCreated,
                     weak_factory_.GetWeakPtr()));
  player_.set_disconnect_handler(base::BindOnce(
      &AVFDemuxer::OnDisconnected, weak_factory_.GetWeakPtr()));
}

void AVFDemuxer::OnPlayerCreated(
    const std::optional<base::UnguessableToken>& overlay_plane_id) {
  if (!overlay_plane_id) {
    FailInitialization("could not create AVPlayer");
    return;
  }
  overlay_plane_id_ = *overlay_plane_id;
  player_->Load(
      base::BindOnce(&AVFDemuxer::OnLoaded, weak_factory_.GetWeakPtr()));
}

void AVFDemuxer::OnLoaded(bool success,
                          mojom::AVFMetadataPtr metadata,
                          const std::string& error) {
  if (!success || !metadata) {
    FailInitialization(error);
    return;
  }
  natural_size_ = metadata->natural_size;
  duration_ = metadata->duration;
  if (metadata->has_audio) {
    streams_.push_back(std::make_unique<PlaceholderStream>(
        media::DemuxerStream::AUDIO, natural_size_));
  }
  if (metadata->has_video) {
    streams_.push_back(std::make_unique<PlaceholderStream>(
        media::DemuxerStream::VIDEO, natural_size_));
  }
  host_->SetDuration(duration_.is_zero() ? media::kInfiniteDuration
                                         : duration_);
  if (init_cb_) {
    std::move(init_cb_).Run(media::PIPELINE_OK);
  }
}

void AVFDemuxer::FailInitialization(const std::string& reason) {
  LOG(ERROR) << "AVFoundation HLS: " << reason;
  if (init_cb_) {
    std::move(init_cb_).Run(media::DEMUXER_ERROR_COULD_NOT_OPEN);
  }
}

void AVFDemuxer::OnDisconnected() {
  if (init_cb_) {
    FailInitialization("AVFoundation media service disconnected");
    return;
  }
  if (observer_) {
    observer_->OnError("AVFoundation media service disconnected");
  } else if (host_) {
    host_->OnDemuxerError(media::PIPELINE_ERROR_DISCONNECTED);
  }
}

void AVFDemuxer::AbortPendingReads() {}
void AVFDemuxer::StartWaitingForSeek(base::TimeDelta seek_time) {}
void AVFDemuxer::CancelPendingSeek(base::TimeDelta seek_time) {}

void AVFDemuxer::Seek(base::TimeDelta time,
                      media::PipelineStatusCallback status_cb) {
  // AVFRenderer seeks the AVPlayer in StartPlayingFrom().
  std::move(status_cb).Run(media::PIPELINE_OK);
}

bool AVFDemuxer::IsSeekable() const {
  return true;
}

void AVFDemuxer::Stop() {
  observer_ = nullptr;
  client_receiver_.reset();
  player_.reset();
  factory_.reset();
}

base::TimeDelta AVFDemuxer::GetStartTime() const {
  return base::TimeDelta();
}

base::Time AVFDemuxer::GetTimelineOffset() const {
  return base::Time();
}

int64_t AVFDemuxer::GetMemoryUsage() const {
  return 0;
}

std::optional<media::container_names::MediaContainerName>
AVFDemuxer::GetContainerForMetrics() const {
  return std::nullopt;
}

void AVFDemuxer::OnTracksChanged(media::DemuxerStream::Type track_type,
                                 std::optional<media::MediaTrack::Id> track_id,
                                 base::TimeDelta curr_time,
                                 TrackChangeCB change_completed_cb) {
  // Track selection isn't supported yet; keep what's playing.
  media::DemuxerStream* stream = nullptr;
  for (auto& s : streams_) {
    if (s->type() == track_type) {
      stream = s.get();
    }
  }
  std::move(change_completed_cb).Run(stream);
}

void AVFDemuxer::SetPlaybackRate(double rate) {}

void AVFDemuxer::OnTimeUpdate(base::TimeDelta media_time, double rate) {
  if (observer_) {
    observer_->OnTimeUpdate(media_time, rate);
  }
}

void AVFDemuxer::OnBufferingStateChange(bool have_enough) {
  if (observer_) {
    observer_->OnBufferingStateChange(have_enough);
  }
}

void AVFDemuxer::OnBufferedRangesChange(
    std::vector<mojom::AVFTimeRangePtr> ranges) {
  if (!host_) {
    return;
  }
  media::Ranges<base::TimeDelta> buffered;
  for (const auto& range : ranges) {
    if (range->end > range->start) {
      buffered.Add(range->start, range->end);
    }
  }
  host_->OnBufferedTimeRangesChanged(buffered);
}

void AVFDemuxer::OnNaturalSizeChange(const gfx::Size& size) {
  natural_size_ = size;
  if (observer_) {
    observer_->OnNaturalSizeChange(size);
  }
}

void AVFDemuxer::OnDurationChange(base::TimeDelta duration) {
  duration_ = duration;
  if (host_ && !init_cb_) {
    host_->SetDuration(duration.is_zero() ? media::kInfiniteDuration
                                          : duration);
  }
}

void AVFDemuxer::OnEnded() {
  if (observer_) {
    observer_->OnEnded();
  }
}

void AVFDemuxer::OnError(const std::string& message) {
  LOG(ERROR) << "AVFoundation HLS: " << message;
  if (observer_) {
    observer_->OnError(message);
  } else if (host_) {
    host_->OnDemuxerError(media::PIPELINE_ERROR_DECODE);
  }
}

}  // namespace compass::avf
