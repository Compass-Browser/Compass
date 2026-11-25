// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_RENDERER_AVF_DEMUXER_H_
#define COMPASS_MEDIA_AVF_RENDERER_AVF_DEMUXER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/unguessable_token.h"
#include "compass/media/avf/public/mojom/avf_player.mojom.h"
#include "media/base/demuxer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/timestamp_constants.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"

namespace compass::avf {

// Receives an AVPlayer's playback events. Implemented by AVFRenderer.
class AVFPlaybackObserver {
 public:
  virtual void OnTimeUpdate(base::TimeDelta media_time, double rate) = 0;
  virtual void OnBufferingStateChange(bool have_enough) = 0;
  virtual void OnNaturalSizeChange(const gfx::Size& size) = 0;
  virtual void OnEnded() = 0;
  virtual void OnError(const std::string& message) = 0;

 protected:
  virtual ~AVFPlaybackObserver() = default;
};

// The media::Demuxer for an HLS URL played by AVPlayer (docs/design/
// fairplay.md). It reads no media: it creates the AVPlayer in the
// AVFoundation utility process, waits for its metadata, and offers the
// pipeline placeholder streams so that it knows there is video and audio.
// AVFRenderer, the only renderer that plays from it, drives the AVPlayer.
//
// Created on the main thread (ContentRendererClient::OverrideDemuxerForUrl),
// used and destroyed on the media thread.
class AVFDemuxer : public media::Demuxer, public mojom::AVFPlayerClient {
 public:
  AVFDemuxer(const GURL& url,
             mojo::PendingRemote<mojom::AVFPlayerFactory> factory);
  AVFDemuxer(const AVFDemuxer&) = delete;
  AVFDemuxer& operator=(const AVFDemuxer&) = delete;
  ~AVFDemuxer() override;

  // The AVFDemuxer `resource` is, or null. Media thread only.
  static AVFDemuxer* FromMediaResource(media::MediaResource* resource);

  mojom::AVFPlayer* player() { return player_.get(); }
  const base::UnguessableToken& overlay_plane_id() const {
    return overlay_plane_id_;
  }
  const gfx::Size& natural_size() const { return natural_size_; }
  base::TimeDelta duration() const { return duration_; }
  bool is_live() const {
    return duration_.is_zero() || duration_ == media::kInfiniteDuration;
  }
  // The AVPlayer's last reported state. A renderer made after a pipeline
  // suspend and resume starts from it, as the AVPlayer only reports changes.
  bool have_enough() const { return have_enough_; }
  base::TimeDelta last_time() const { return last_time_; }
  void SetObserver(AVFPlaybackObserver* observer);

  // media::Demuxer:
  std::vector<raw_ptr<media::DemuxerStream>> GetAllStreams() override;
  std::string GetDisplayName() const override;
  media::DemuxerType GetDemuxerType() const override;
  void Initialize(media::DemuxerHost* host,
                  media::PipelineStatusCallback status_cb) override;
  void AbortPendingReads() override;
  void StartWaitingForSeek(base::TimeDelta seek_time) override;
  void CancelPendingSeek(base::TimeDelta seek_time) override;
  void Seek(base::TimeDelta time,
            media::PipelineStatusCallback status_cb) override;
  bool IsSeekable() const override;
  void Stop() override;
  base::TimeDelta GetStartTime() const override;
  base::Time GetTimelineOffset() const override;
  int64_t GetMemoryUsage() const override;
  std::optional<media::container_names::MediaContainerName>
  GetContainerForMetrics() const override;
  void OnTracksChanged(media::DemuxerStream::Type track_type,
                       std::optional<media::MediaTrack::Id> track_id,
                       base::TimeDelta curr_time,
                       TrackChangeCB change_completed_cb) override;
  void SetPlaybackRate(double rate) override;
  void SetEncryptedMediaInitDataCB(EncryptedMediaInitDataCB cb) override;

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
  class PlaceholderStream;

  void OnPlayerCreated(
      const std::optional<base::UnguessableToken>& overlay_plane_id);
  void OnLoaded(bool success,
                mojom::AVFMetadataPtr metadata,
                const std::string& error);
  void OnDisconnected();
  void FailInitialization(const std::string& reason);
  void FallBackToChromiumHls();

  const GURL url_;
  mojo::PendingRemote<mojom::AVFPlayerFactory> pending_factory_;
  mojo::Remote<mojom::AVFPlayerFactory> factory_;
  mojo::Remote<mojom::AVFPlayer> player_;
  mojo::Receiver<mojom::AVFPlayerClient> client_receiver_{this};

  raw_ptr<media::DemuxerHost> host_ = nullptr;
  media::PipelineStatusCallback init_cb_;
  raw_ptr<AVFPlaybackObserver> observer_ = nullptr;
  EncryptedMediaInitDataCB encrypted_cb_;

  base::UnguessableToken overlay_plane_id_;
  gfx::Size natural_size_;
  base::TimeDelta duration_;
  bool have_enough_ = false;
  base::TimeDelta last_time_;
  std::vector<std::unique_ptr<PlaceholderStream>> streams_;

  base::WeakPtrFactory<AVFDemuxer> weak_factory_{this};
};

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_RENDERER_AVF_DEMUXER_H_
