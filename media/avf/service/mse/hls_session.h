// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_SERVICE_MSE_HLS_SESSION_H_
#define COMPASS_MEDIA_AVF_SERVICE_MSE_HLS_SESSION_H_

#include <stdint.h>

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/memory/ref_counted_memory.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "compass/media/avf/service/mse/mp4_writer.h"
#include "compass/media/avf/service/mse/sample_conversion.h"
#include "media/base/audio_decoder_config.h"
#include "media/base/decrypt_config.h"
#include "media/base/video_decoder_config.h"

namespace compass::avf::mse {

// One sample from the renderer's demuxer stream (a DecoderBuffer).
struct MseSample {
  MseSample();
  MseSample(MseSample&&);
  MseSample& operator=(MseSample&&);
  ~MseSample();

  std::vector<uint8_t> data;
  base::TimeDelta timestamp;
  base::TimeDelta duration;
  bool is_key_frame = false;
  std::unique_ptr<media::DecryptConfig> decrypt_config;
};

// An MSE stream from one playback position on, as an HLS playlist of fMP4
// segments AVPlayer reads over loopback (docs/design/fairplay.md "FairPlay
// for MSE"). Samples arrive in decode order per track; segments are cut at
// video keyframes once audio has caught up, with both tracks in each.
//
// The timeline is rebased: AVPlayer's time 0 is origin(), the first
// segment's start. Fed on one sequence; Playlist() and Resource() may be
// called from any thread (the loopback server's).
class HlsSession : public base::RefCountedThreadSafe<HlsSession> {
 public:
  enum class Track { kAudio, kVideo };

  // Segments are cut at the first video keyframe this long after the last
  // cut (or at one with new parameter sets or a new config). Longer GOPs than
  // the TARGETDURATION would make segments AVPlayer may refuse.
  static constexpr base::TimeDelta kMinSegment = base::Seconds(2);
  static constexpr int kTargetDuration = 10;

  HlsSession(std::optional<media::AudioDecoderConfig> audio,
             std::optional<media::VideoDecoderConfig> video);
  HlsSession(const HlsSession&) = delete;
  HlsSession& operator=(const HlsSession&) = delete;

  void SetAudioConfig(const media::AudioDecoderConfig& config);
  void SetVideoConfig(const media::VideoDecoderConfig& config);
  void Append(Track track, std::vector<MseSample> samples);
  void EndOfStream(Track track);

  // Empty unless the stream can't be played (codec, encryption scheme...).
  const std::string& error() const { return error_; }
  // The media time of AVPlayer's 0; set with the first segment.
  std::optional<base::TimeDelta> origin() const { return origin_; }
  size_t segment_count() const;
  bool ended() const;
  // The media time segments reach.
  base::TimeDelta segmented_end() const { return cut_time_; }

  // Frees the bytes of segments that end before `media_time` (AVPlayer has
  // played them).
  void DropBefore(base::TimeDelta media_time);

  // The playlist; null until there's a segment. Any thread.
  std::optional<std::string> Playlist() const;
  // "init-N.mp4" or "seg-N.m4s"; null if unknown or dropped. Any thread.
  scoped_refptr<base::RefCountedBytes> Resource(
      std::string_view name) const;

 private:
  friend class base::RefCountedThreadSafe<HlsSession>;
  ~HlsSession();

  struct PendingSample {
    MseSample sample;
    // Video: the parameter sets this keyframe carried, if any.
    std::optional<ParameterSets> params;
    bool config_changed = false;
  };
  struct TrackState {
    TrackState();
    ~TrackState();
    std::deque<PendingSample> pending;
    bool ended = false;
    bool config_changed = false;
    std::optional<std::vector<uint8_t>> audio_specific_config;
  };
  struct Segment {
    double duration = 0;
    size_t init = 0;
    std::string key_uri;  // Empty: clear.
    // Clear to encrypted or back (a clear lead): AVPlayer takes the new
    // init segment and key after a discontinuity.
    bool discontinuity = false;
    base::TimeDelta end;
    scoped_refptr<base::RefCountedBytes> bytes;
  };
  // What an init segment describes; a change needs a new one.
  struct InitKey {
    std::optional<ParameterSets> params;
    std::optional<Mp4Encryption> video_encryption;
    std::optional<Mp4Encryption> audio_encryption;
    std::optional<std::vector<uint8_t>> audio_config;
    int video_config_generation = 0;
    int audio_config_generation = 0;
    bool operator==(const InitKey&) const;
  };

  void Fail(const std::string& error);
  bool ConvertVideo(PendingSample& pending);
  bool ConvertAudio(PendingSample& pending);
  // Emits every segment that can be cut now.
  void Cut();
  void EmitSegment(size_t video_count, base::TimeDelta cut_time);
  std::vector<uint8_t> MakeInit(const InitKey& key);

  std::optional<media::AudioDecoderConfig> audio_config_;
  std::optional<media::VideoDecoderConfig> video_config_;
  int audio_config_generation_ = 0;
  int video_config_generation_ = 0;
  TrackState audio_;
  TrackState video_;
  ParameterSets params_;  // The current segment's.
  std::string error_;

  std::optional<base::TimeDelta> origin_;
  base::TimeDelta cut_time_;  // The end of the last segment.
  std::optional<InitKey> init_key_;
  std::optional<Mp4Encryption> video_encryption_;
  std::optional<Mp4Encryption> audio_encryption_;
  uint32_t sequence_number_ = 1;

  mutable base::Lock lock_;
  std::vector<Segment> segments_ GUARDED_BY(lock_);
  std::vector<scoped_refptr<base::RefCountedBytes>> inits_
      GUARDED_BY(lock_);
  bool playlist_ended_ GUARDED_BY(lock_) = false;
};

}  // namespace compass::avf::mse

#endif  // COMPASS_MEDIA_AVF_SERVICE_MSE_HLS_SESSION_H_
