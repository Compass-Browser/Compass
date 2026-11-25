// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/mse/hls_session.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "media/base/channel_layout.h"
#include "media/base/encryption_scheme.h"

namespace compass::avf::mse {
namespace {

constexpr uint32_t kVideoTrackId = 1;
constexpr uint32_t kAudioTrackId = 2;
constexpr uint32_t kVideoTimescale = 90000;

int64_t Ticks(base::TimeDelta time, uint32_t timescale) {
  return std::llround(time.InMicrosecondsF() * timescale /
                      base::Time::kMicrosecondsPerSecond);
}

std::string KeyUri(const std::array<uint8_t, 16>& key_id) {
  return "skd://" + base::ToLowerASCII(base::HexEncode(key_id));
}

std::unique_ptr<media::DecryptConfig> WithSubsamples(
    const media::DecryptConfig& config,
    std::vector<media::SubsampleEntry> subsamples) {
  return media::DecryptConfig::CreateCbcsConfig(
      config.key_id(), config.iv(), std::move(subsamples),
      config.encryption_pattern());
}

// A segment's samples share one key and either one IV (constant, as 'cbcs'
// content usually has) or one per sample.
std::optional<Mp4Encryption> EncryptionFor(
    const std::vector<const MseSample*>& samples) {
  std::optional<Mp4Encryption> encryption;
  bool constant = true;
  std::string first_iv;
  for (const MseSample* sample : samples) {
    const media::DecryptConfig* config = sample->decrypt_config.get();
    if (!config) {
      continue;
    }
    if (!encryption) {
      encryption.emplace();
      std::copy_n(config->key_id().begin(),
                  std::min<size_t>(16, config->key_id().size()),
                  encryption->key_id.begin());
      if (config->encryption_pattern()) {
        encryption->crypt_byte_block =
            config->encryption_pattern()->crypt_byte_block();
        encryption->skip_byte_block =
            config->encryption_pattern()->skip_byte_block();
      }
      first_iv = config->iv();
    } else if (config->iv() != first_iv) {
      constant = false;
    }
  }
  if (encryption && constant) {
    encryption->constant_iv.assign(first_iv.begin(), first_iv.end());
  }
  return encryption;
}

bool SameEncryption(const std::optional<Mp4Encryption>& a,
                    const std::optional<Mp4Encryption>& b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  return !a || (a->key_id == b->key_id && a->constant_iv == b->constant_iv &&
                a->crypt_byte_block == b->crypt_byte_block &&
                a->skip_byte_block == b->skip_byte_block);
}

Mp4Sample ToMp4Sample(const MseSample& sample,
                      const std::optional<Mp4Encryption>& encryption) {
  Mp4Sample out;
  out.data = sample.data;
  out.sync = sample.is_key_frame;
  if (encryption) {
    if (const media::DecryptConfig* config = sample.decrypt_config.get()) {
      if (encryption->constant_iv.empty()) {
        out.iv.assign(config->iv().begin(), config->iv().end());
      }
      out.subsamples = config->subsamples();
    } else {
      // A clear sample in an encrypted track: all of it clear.
      if (encryption->constant_iv.empty()) {
        out.iv.assign(16, 0);
      }
      out.subsamples = {{static_cast<uint32_t>(sample.data.size()), 0}};
    }
    // senc's clear byte counts are 16-bit.
    std::vector<media::SubsampleEntry> split;
    for (auto entry : out.subsamples) {
      while (entry.clear_bytes > 0xFFFF) {
        split.push_back({0xFFFF, 0});
        entry.clear_bytes -= 0xFFFF;
      }
      split.push_back(entry);
    }
    out.subsamples = std::move(split);
  }
  return out;
}

}  // namespace

MseSample::MseSample() = default;
MseSample::MseSample(MseSample&&) = default;
MseSample& MseSample::operator=(MseSample&&) = default;
MseSample::~MseSample() = default;

HlsSession::TrackState::TrackState() = default;
HlsSession::TrackState::~TrackState() = default;

bool HlsSession::InitKey::operator==(const InitKey& other) const {
  return params == other.params &&
         SameEncryption(video_encryption, other.video_encryption) &&
         SameEncryption(audio_encryption, other.audio_encryption) &&
         audio_config == other.audio_config &&
         video_config_generation == other.video_config_generation &&
         audio_config_generation == other.audio_config_generation;
}

HlsSession::HlsSession(std::optional<media::AudioDecoderConfig> audio,
                       std::optional<media::VideoDecoderConfig> video) {
  if (video) {
    SetVideoConfig(*video);
    video_.config_changed = false;
  }
  if (audio) {
    SetAudioConfig(*audio);
    audio_.config_changed = false;
  }
  if (!audio && !video) {
    Fail("No audio or video.");
  }
}

HlsSession::~HlsSession() = default;

void HlsSession::Fail(const std::string& error) {
  if (error_.empty()) {
    error_ = error;
  }
}

void HlsSession::SetAudioConfig(const media::AudioDecoderConfig& config) {
  if (config.codec() != media::AudioCodec::kAAC) {
    Fail("FairPlay through MSE plays AAC audio only, not " +
         media::GetCodecName(config.codec()) + ".");
    return;
  }
  audio_config_ = config;
  ++audio_config_generation_;
  audio_.config_changed = true;
  if (!config.extra_data().empty()) {
    audio_.audio_specific_config = config.extra_data();
  }
}

void HlsSession::SetVideoConfig(const media::VideoDecoderConfig& config) {
  if (config.codec() != media::VideoCodec::kH264 &&
      config.codec() != media::VideoCodec::kHEVC) {
    Fail("FairPlay through MSE plays H.264 and HEVC video only, not " +
         media::GetCodecName(config.codec()) + ".");
    return;
  }
  video_config_ = config;
  ++video_config_generation_;
  video_.config_changed = true;
}

void HlsSession::Append(Track track, std::vector<MseSample> samples) {
  TrackState& state = track == Track::kVideo ? video_ : audio_;
  for (auto& sample : samples) {
    if (!error_.empty()) {
      return;
    }
    if (sample.decrypt_config &&
        sample.decrypt_config->encryption_scheme() !=
            media::EncryptionScheme::kCbcs) {
      Fail("FairPlay plays 'cbcs' content only, not 'cenc'.");
      return;
    }
    PendingSample pending;
    pending.sample = std::move(sample);
    pending.config_changed = state.config_changed;
    state.config_changed = false;
    bool converted = track == Track::kVideo ? ConvertVideo(pending)
                                            : ConvertAudio(pending);
    if (!converted) {
      Fail("Can't repackage a sample for AVPlayer.");
      return;
    }
    state.pending.push_back(std::move(pending));
  }
  Cut();
}

void HlsSession::EndOfStream(Track track) {
  (track == Track::kVideo ? video_ : audio_).ended = true;
  Cut();
}

bool HlsSession::ConvertVideo(PendingSample& pending) {
  if (!video_config_) {
    return false;
  }
  MseSample& sample = pending.sample;
  std::vector<media::SubsampleEntry> subsamples;
  if (sample.decrypt_config) {
    subsamples = sample.decrypt_config->subsamples();
  }
  ParameterSets found;
  NalCodec codec = video_config_->codec() == media::VideoCodec::kH264
                       ? NalCodec::kH264
                       : NalCodec::kHevc;
  if (!AnnexBToLengthPrefixed(codec, sample.data, subsamples, found)) {
    return false;
  }
  if (sample.decrypt_config) {
    sample.decrypt_config =
        WithSubsamples(*sample.decrypt_config, std::move(subsamples));
  }
  if (!found.sps.empty() || !found.pps.empty() || !found.vps.empty()) {
    pending.params = std::move(found);
  }
  return true;
}

bool HlsSession::ConvertAudio(PendingSample& pending) {
  MseSample& sample = pending.sample;
  if (!audio_.audio_specific_config) {
    audio_.audio_specific_config = AudioSpecificConfigFromAdts(sample.data);
    if (!audio_.audio_specific_config) {
      return false;
    }
  }
  std::vector<media::SubsampleEntry> subsamples;
  if (sample.decrypt_config) {
    subsamples = sample.decrypt_config->subsamples();
  }
  if (StripAdts(sample.data, subsamples) && sample.decrypt_config) {
    sample.decrypt_config =
        WithSubsamples(*sample.decrypt_config, std::move(subsamples));
  }
  return true;
}

void HlsSession::Cut() {
  while (error_.empty()) {
    if (!origin_) {
      if (video_config_) {
        // Start at a keyframe.
        while (!video_.pending.empty() &&
               !video_.pending.front().sample.is_key_frame) {
          video_.pending.pop_front();
        }
        if (video_.pending.empty()) {
          return;
        }
        origin_ = video_.pending.front().sample.timestamp;
      } else {
        if (audio_.pending.empty()) {
          return;
        }
        origin_ = audio_.pending.front().sample.timestamp;
      }
      cut_time_ = *origin_;
    }
    // Audio from before the segments reach is too late for them.
    while (!audio_.pending.empty() &&
           audio_.pending.front().sample.timestamp < cut_time_) {
      audio_.pending.pop_front();
    }

    size_t video_count = 0;
    base::TimeDelta cut;
    bool ready = false;
    if (video_config_ && !video_.pending.empty()) {
      const ParameterSets* current =
          video_.pending.front().params ? &*video_.pending.front().params
                                        : &params_;
      for (size_t k = 1; k < video_.pending.size(); ++k) {
        const PendingSample& candidate = video_.pending[k];
        if (!candidate.sample.is_key_frame) {
          continue;
        }
        bool new_params = candidate.params && *candidate.params != *current;
        if (candidate.sample.timestamp - cut_time_ >= kMinSegment ||
            new_params || candidate.config_changed) {
          video_count = k;
          cut = candidate.sample.timestamp;
          ready = true;
          break;
        }
      }
      if (!ready && video_.ended) {
        video_count = video_.pending.size();
        cut = cut_time_;
        for (const auto& pending : video_.pending) {
          cut = std::max(cut,
                         pending.sample.timestamp + pending.sample.duration);
        }
        ready = true;
      }
      // Audio has to reach the cut.
      if (ready && audio_config_ && !audio_.ended) {
        ready = !audio_.pending.empty() &&
                audio_.pending.back().sample.timestamp +
                        audio_.pending.back().sample.duration >=
                    cut;
      }
    } else if (!video_config_ || video_.ended) {
      // Audio only, or the audio after the video ended.
      for (const auto& pending : audio_.pending) {
        if (pending.sample.timestamp - cut_time_ >= kMinSegment) {
          cut = pending.sample.timestamp;
          ready = true;
          break;
        }
      }
      if (!ready && audio_.ended && !audio_.pending.empty()) {
        const MseSample& last = audio_.pending.back().sample;
        cut = last.timestamp + last.duration;
        ready = true;
      }
    }

    if (!ready) {
      bool done = (!video_config_ || (video_.ended && video_.pending.empty())) &&
                  (!audio_config_ || (audio_.ended && audio_.pending.empty()));
      if (done) {
        base::AutoLock lock(lock_);
        playlist_ended_ = true;
      }
      return;
    }
    EmitSegment(video_count, cut);
  }
}

void HlsSession::EmitSegment(size_t video_count, base::TimeDelta cut_time) {
  std::vector<PendingSample> video;
  for (size_t i = 0; i < video_count; ++i) {
    video.push_back(std::move(video_.pending.front()));
    video_.pending.pop_front();
  }
  std::vector<PendingSample> audio;
  while (!audio_.pending.empty() &&
         audio_.pending.front().sample.timestamp < cut_time) {
    audio.push_back(std::move(audio_.pending.front()));
    audio_.pending.pop_front();
  }
  if (!video.empty() && video.front().params) {
    params_ = *video.front().params;
  }

  std::vector<const MseSample*> video_samples, audio_samples;
  for (const auto& pending : video) {
    video_samples.push_back(&pending.sample);
  }
  for (const auto& pending : audio) {
    audio_samples.push_back(&pending.sample);
  }
  // Keep the track's protection unless this segment's samples say otherwise.
  if (std::optional<Mp4Encryption> encryption = EncryptionFor(video_samples)) {
    video_encryption_ = encryption;
  }
  if (std::optional<Mp4Encryption> encryption = EncryptionFor(audio_samples)) {
    audio_encryption_ = encryption;
  }

  InitKey key;
  if (video_config_) {
    key.params = params_;
    key.video_encryption = video_encryption_;
    key.video_config_generation = video_config_generation_;
  }
  if (audio_config_) {
    key.audio_encryption = audio_encryption_;
    key.audio_config = audio_.audio_specific_config;
    key.audio_config_generation = audio_config_generation_;
  }
  scoped_refptr<base::RefCountedBytes> new_init;
  if (!init_key_ || !(*init_key_ == key)) {
    std::vector<uint8_t> init = MakeInit(key);
    if (!error_.empty()) {
      return;
    }
    new_init = base::MakeRefCounted<base::RefCountedBytes>(std::move(init));
    init_key_ = key;
  }

  std::vector<Mp4Fragment> fragments;
  if (!video.empty()) {
    Mp4Fragment fragment;
    fragment.track_id = kVideoTrackId;
    fragment.encrypted = video_encryption_.has_value();
    fragment.per_sample_iv_size =
        video_encryption_ && video_encryption_->constant_iv.empty() ? 16 : 0;
    // Decode times: the presentation times in order (the samples come in
    // decode order; DecoderBuffers have no decode time).
    std::vector<int64_t> pts;
    for (const auto& pending : video) {
      pts.push_back(
          Ticks(pending.sample.timestamp - *origin_, kVideoTimescale));
    }
    std::vector<int64_t> dts = pts;
    std::sort(dts.begin(), dts.end());
    fragment.base_decode_time = std::max<int64_t>(0, dts.front());
    for (size_t i = 0; i < video.size(); ++i) {
      Mp4Sample sample = ToMp4Sample(video[i].sample, video_encryption_);
      int64_t duration =
          i + 1 < video.size()
              ? dts[i + 1] - dts[i]
              : Ticks(video[i].sample.duration, kVideoTimescale);
      if (duration <= 0 && i > 0) {
        duration = dts[i] - dts[i - 1];
      }
      sample.duration = static_cast<uint32_t>(std::max<int64_t>(1, duration));
      sample.composition_offset = static_cast<int32_t>(pts[i] - dts[i]);
      fragment.samples.push_back(std::move(sample));
    }
    fragments.push_back(std::move(fragment));
  }
  if (!audio.empty()) {
    uint32_t timescale = audio_config_->samples_per_second();
    Mp4Fragment fragment;
    fragment.track_id = kAudioTrackId;
    fragment.encrypted = audio_encryption_.has_value();
    fragment.per_sample_iv_size =
        audio_encryption_ && audio_encryption_->constant_iv.empty() ? 16 : 0;
    fragment.base_decode_time = std::max<int64_t>(
        0, Ticks(audio.front().sample.timestamp - *origin_, timescale));
    for (size_t i = 0; i < audio.size(); ++i) {
      Mp4Sample sample = ToMp4Sample(audio[i].sample, audio_encryption_);
      sample.sync = true;
      int64_t duration = Ticks(audio[i].sample.duration, timescale);
      if (i + 1 < audio.size() && duration <= 0) {
        duration = Ticks(audio[i + 1].sample.timestamp -
                             audio[i].sample.timestamp,
                         timescale);
      }
      sample.duration = static_cast<uint32_t>(std::max<int64_t>(1, duration));
      fragment.samples.push_back(std::move(sample));
    }
    fragments.push_back(std::move(fragment));
  }

  if (VLOG_IS_ON(1)) {
    size_t encrypted_video = 0, encrypted_audio = 0;
    std::string video_kids;
    for (const MseSample* sample : video_samples) {
      if (sample->decrypt_config) {
        ++encrypted_video;
        std::string kid = base::HexEncode(sample->decrypt_config->key_id());
        if (video_kids.find(kid) == std::string::npos) {
          video_kids += kid + " ";
        }
      }
    }
    for (const MseSample* sample : audio_samples) {
      encrypted_audio += sample->decrypt_config ? 1 : 0;
    }
    VLOG(1) << "HLS segment " << (sequence_number_ - 1) << ": "
            << cut_time_.InSecondsF() << "-" << cut_time.InSecondsF()
            << "s video " << encrypted_video << "/" << video_samples.size()
            << " encrypted (" << video_kids << ") audio " << encrypted_audio
            << "/" << audio_samples.size() << " init "
            << (new_init ? "new" : "same") << " video protection "
            << (video_encryption_ ? "yes" : "no");
  }

  Segment segment;
  segment.duration = (cut_time - cut_time_).InSecondsF();
  segment.end = cut_time;
  if (video_encryption_ && video_config_) {
    segment.key_uri = KeyUri(video_encryption_->key_id);
  } else if (audio_encryption_ && audio_config_) {
    segment.key_uri = KeyUri(audio_encryption_->key_id);
  }
  segment.bytes = base::MakeRefCounted<base::RefCountedBytes>(
      WriteMediaSegment(sequence_number_++, fragments));
  cut_time_ = cut_time;

  base::AutoLock lock(lock_);
  if (new_init) {
    inits_.push_back(std::move(new_init));
  }
  segment.init = inits_.size() - 1;
  segment.discontinuity =
      !segments_.empty() &&
      segments_.back().key_uri.empty() != segment.key_uri.empty();
  segments_.push_back(std::move(segment));
}

std::vector<uint8_t> HlsSession::MakeInit(const InitKey& key) {
  std::vector<Mp4Track> tracks;
  if (video_config_) {
    Mp4Track track;
    track.kind = Mp4Track::Kind::kVideo;
    track.track_id = kVideoTrackId;
    track.timescale = kVideoTimescale;
    bool h264 = video_config_->codec() == media::VideoCodec::kH264;
    track.format = h264 ? FourCC("avc1") : FourCC("hvc1");
    std::optional<std::vector<uint8_t>> config =
        key.params ? (h264 ? MakeAvcC(*key.params) : MakeHvcC(*key.params))
                   : std::nullopt;
    if (!config) {
      Fail("The video has no usable parameter sets.");
      return {};
    }
    track.config_box = std::move(*config);
    gfx::Size size = video_config_->visible_rect().size();
    track.width = static_cast<uint16_t>(size.width());
    track.height = static_cast<uint16_t>(size.height());
    track.encryption = key.video_encryption;
    tracks.push_back(std::move(track));
  }
  if (audio_config_) {
    if (!key.audio_config) {
      Fail("The audio has no AudioSpecificConfig.");
      return {};
    }
    Mp4Track track;
    track.kind = Mp4Track::Kind::kAudio;
    track.track_id = kAudioTrackId;
    track.timescale = audio_config_->samples_per_second();
    track.format = FourCC("mp4a");
    track.config_box = MakeEsds(*key.audio_config);
    track.channels = static_cast<uint16_t>(audio_config_->channels());
    track.sample_rate = audio_config_->samples_per_second();
    track.encryption = key.audio_encryption;
    tracks.push_back(std::move(track));
  }
  return WriteInitSegment(tracks);
}

size_t HlsSession::segment_count() const {
  base::AutoLock lock(lock_);
  return segments_.size();
}

bool HlsSession::ended() const {
  base::AutoLock lock(lock_);
  return playlist_ended_;
}

void HlsSession::DropBefore(base::TimeDelta media_time) {
  base::AutoLock lock(lock_);
  for (auto& segment : segments_) {
    if (segment.end < media_time) {
      segment.bytes.reset();
    }
  }
}

std::optional<std::string> HlsSession::Playlist() const {
  base::AutoLock lock(lock_);
  if (segments_.empty()) {
    return std::nullopt;
  }
  std::string out = base::StringPrintf(
      "#EXTM3U\n"
      "#EXT-X-VERSION:7\n"
      "#EXT-X-TARGETDURATION:%d\n"
      "#EXT-X-MEDIA-SEQUENCE:0\n"
      "#EXT-X-PLAYLIST-TYPE:EVENT\n"
      "#EXT-X-START:TIME-OFFSET=0,PRECISE=YES\n",
      kTargetDuration);
  size_t init = SIZE_MAX;
  std::string key;
  for (size_t i = 0; i < segments_.size(); ++i) {
    const Segment& segment = segments_[i];
    if (segment.discontinuity) {
      out += "#EXT-X-DISCONTINUITY\n";
    }
    if (segment.init != init) {
      init = segment.init;
      base::StringAppendF(&out, "#EXT-X-MAP:URI=\"init-%zu.mp4\"\n", init);
    }
    if (segment.key_uri != key) {
      key = segment.key_uri;
      if (key.empty()) {
        out += "#EXT-X-KEY:METHOD=NONE\n";
      } else {
        base::StringAppendF(
            &out,
            "#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"%s\","
            "KEYFORMAT=\"com.apple.streamingkeydelivery\","
            "KEYFORMATVERSIONS=\"1\"\n",
            key.c_str());
      }
    }
    base::StringAppendF(&out, "#EXTINF:%.6f,\nseg-%zu.m4s\n", segment.duration,
                        i);
  }
  if (playlist_ended_) {
    out += "#EXT-X-ENDLIST\n";
  }
  return out;
}

scoped_refptr<base::RefCountedBytes> HlsSession::Resource(
    std::string_view name) const {
  auto index_of = [&](std::string_view prefix,
                      std::string_view suffix) -> std::optional<size_t> {
    if (!base::StartsWith(name, prefix) || !base::EndsWith(name, suffix)) {
      return std::nullopt;
    }
    size_t index;
    if (!base::StringToSizeT(name.substr(prefix.size(), name.size() -
                                                            prefix.size() -
                                                            suffix.size()),
                             &index)) {
      return std::nullopt;
    }
    return index;
  };
  base::AutoLock lock(lock_);
  if (auto index = index_of("init-", ".mp4"); index && *index < inits_.size()) {
    return inits_[*index];
  }
  if (auto index = index_of("seg-", ".m4s");
      index && *index < segments_.size()) {
    return segments_[*index].bytes;
  }
  return nullptr;
}

}  // namespace compass::avf::mse
