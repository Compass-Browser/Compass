// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/mse/hls_session.h"

#include <memory>

#include "base/containers/span.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "media/base/channel_layout.h"
#include "media/base/media_util.h"
#include "media/base/video_transformation.h"
#include "media/formats/mp4/box_definitions.h"
#include "media/formats/mp4/box_reader.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::avf::mse {
namespace {

std::vector<uint8_t> Hex(std::string_view hex) {
  std::vector<uint8_t> bytes;
  CHECK(base::HexStringToBytes(hex, &bytes));
  return bytes;
}

const char kSps[] = "6764000dacd941419f9f0110000003001000000303c0f1429960";
const char kPps[] = "68ebe3cb22c0";
constexpr char kKeyId[] = "0123456789abcdef";

media::VideoDecoderConfig VideoConfig() {
  return media::VideoDecoderConfig(
      media::VideoCodec::kH264, media::H264PROFILE_HIGH,
      media::VideoDecoderConfig::AlphaMode::kIsOpaque,
      media::VideoColorSpace::REC709(), media::kNoTransformation,
      gfx::Size(320, 180), gfx::Rect(320, 180), gfx::Size(320, 180),
      media::EmptyExtraData(), media::EncryptionScheme::kCbcs);
}

media::AudioDecoderConfig AudioConfig() {
  return media::AudioDecoderConfig(
      media::AudioCodec::kAAC, media::kSampleFormatPlanarF32,
      media::ChannelLayoutConfig::Stereo(), 48000, {0x11, 0x90},
      media::EncryptionScheme::kUnencrypted);
}

// A 30 fps H.264 frame as Chromium's parser makes it: Annex B, parameter
// sets before keyframes; encrypted ('cbcs') when `encrypted`.
MseSample VideoFrame(int index, bool key, bool encrypted) {
  MseSample sample;
  std::vector<uint8_t> start = {0, 0, 0, 1};
  std::vector<uint8_t> prefix;
  if (key) {
    for (const char* nal : {kSps, kPps}) {
      prefix.insert(prefix.end(), start.begin(), start.end());
      std::vector<uint8_t> bytes = Hex(nal);
      prefix.insert(prefix.end(), bytes.begin(), bytes.end());
    }
  }
  prefix.insert(prefix.end(), start.begin(), start.end());
  prefix.insert(prefix.end(), {static_cast<uint8_t>(key ? 0x65 : 0x41), 0x88});
  sample.data = prefix;
  sample.data.insert(sample.data.end(), 64, 0x5A);
  sample.timestamp = base::Microseconds(index * 1000000 / 30);
  sample.duration = base::Microseconds(1000000 / 30);
  sample.is_key_frame = key;
  if (encrypted) {
    sample.decrypt_config = media::DecryptConfig::CreateCbcsConfig(
        kKeyId, std::string(16, 'i'),
        {{static_cast<uint32_t>(prefix.size()), 64}},
        media::EncryptionPattern::Create(1, 9));
  }
  return sample;
}

// 1024-sample AAC frames at 48 kHz, with an ADTS header.
MseSample AudioFrame(int index) {
  MseSample sample;
  sample.data = {0xFF, 0xF1, 0x4C, 0x80, 0x01, 0xFF, 0xFC};
  sample.data.insert(sample.data.end(), 20, 0x21);
  sample.timestamp = base::Microseconds(index * 1024 * 1000000ll / 48000);
  sample.duration = base::Microseconds(1024 * 1000000ll / 48000);
  sample.is_key_frame = true;
  return sample;
}

// `seconds` of 30 fps video with a keyframe every second, and audio.
void Feed(HlsSession& session, int from_frame, int frames, bool encrypted) {
  std::vector<MseSample> video;
  for (int i = from_frame; i < from_frame + frames; ++i) {
    video.push_back(VideoFrame(i, i % 30 == 0, encrypted));
  }
  session.Append(HlsSession::Track::kVideo, std::move(video));
  std::vector<MseSample> audio;
  int first = from_frame * 48000 / 30 / 1024;
  int last = (from_frame + frames) * 48000 / 30 / 1024;
  for (int i = first; i < last; ++i) {
    audio.push_back(AudioFrame(i));
  }
  session.Append(HlsSession::Track::kAudio, std::move(audio));
}

TEST(HlsSessionTest, SegmentsAtKeyframes) {
  auto session = base::MakeRefCounted<HlsSession>(AudioConfig(), VideoConfig());
  EXPECT_FALSE(session->Playlist());
  Feed(*session, 0, 30 * 5, /*encrypted=*/true);
  ASSERT_TRUE(session->error().empty()) << session->error();
  ASSERT_TRUE(session->origin());
  EXPECT_EQ(*session->origin(), base::TimeDelta());
  // 2 s segments; the last two seconds wait for the next keyframe.
  EXPECT_EQ(session->segment_count(), 2u);

  std::optional<std::string> playlist = session->Playlist();
  ASSERT_TRUE(playlist);
  EXPECT_NE(playlist->find("#EXT-X-PLAYLIST-TYPE:EVENT"), std::string::npos);
  EXPECT_NE(playlist->find("#EXT-X-MAP:URI=\"init-0.mp4\""), std::string::npos);
  EXPECT_NE(playlist->find("#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://"
                           "30313233343536373839616263646566\""),
            std::string::npos);
  EXPECT_NE(playlist->find("#EXTINF:2.000000,\nseg-0.m4s"), std::string::npos);
  EXPECT_EQ(playlist->find("#EXT-X-ENDLIST"), std::string::npos);

  session->EndOfStream(HlsSession::Track::kVideo);
  session->EndOfStream(HlsSession::Track::kAudio);
  EXPECT_TRUE(session->ended());
  EXPECT_EQ(session->segment_count(), 3u);
  EXPECT_NE(session->Playlist()->find("#EXT-X-ENDLIST"), std::string::npos);

  // The init segment's video track: encrypted H.264 with the stream's key.
  scoped_refptr<base::RefCountedBytes> init = session->Resource("init-0.mp4");
  ASSERT_TRUE(init);
  const std::vector<uint8_t>& bytes = init->as_vector();
  size_t ftyp = (bytes[2] << 8) | bytes[3];
  media::NullMediaLog log;
  std::unique_ptr<media::mp4::BoxReader> reader;
  ASSERT_EQ(media::mp4::BoxReader::ReadTopLevelBox(
                base::span(bytes).subspan(ftyp), &log, &reader),
            media::mp4::ParseResult::kOk);
  media::mp4::Movie moov;
  ASSERT_TRUE(moov.Parse(reader.get()));
  ASSERT_EQ(moov.tracks.size(), 2u);
  const auto& entry = moov.tracks[0]
                          .media.information.sample_table.description
                          .video_entries[0];
  EXPECT_EQ(entry.sinf.format.format, media::mp4::FOURCC_AVC1);
  EXPECT_EQ(std::string(entry.sinf.info.track_encryption.default_kid.begin(),
                        entry.sinf.info.track_encryption.default_kid.end()),
            std::string(kKeyId));

  // A media segment: video samples in AVCC, decode times from 0.
  scoped_refptr<base::RefCountedBytes> segment = session->Resource("seg-0.m4s");
  ASSERT_TRUE(segment);
  ASSERT_EQ(media::mp4::BoxReader::ReadTopLevelBox(
                base::span(segment->as_vector()), &log, &reader),
            media::mp4::ParseResult::kOk);
  media::mp4::MovieFragment moof;
  ASSERT_TRUE(moof.Parse(reader.get()));
  ASSERT_EQ(moof.tracks.size(), 2u);
  EXPECT_EQ(moof.tracks[0].decode_time.decode_time, 0u);
  EXPECT_EQ(moof.tracks[0].runs[0].sample_count, 60u);
  // Keyframe: length prefix + 2 header bytes + 64; no parameter sets.
  EXPECT_EQ(moof.tracks[0].runs[0].sample_sizes[0], 4u + 2u + 64u);
  EXPECT_EQ(moof.tracks[1].decode_time.decode_time, 0u);
  EXPECT_FALSE(session->Resource("seg-9.m4s"));
}

TEST(HlsSessionTest, StartsAtAKeyframe) {
  auto session = base::MakeRefCounted<HlsSession>(AudioConfig(), VideoConfig());
  // A session from 1.5 s on: frames from 1.5 s, the first keyframe at 2 s.
  Feed(*session, 45, 30 * 4, /*encrypted=*/false);
  ASSERT_TRUE(session->origin());
  EXPECT_EQ(*session->origin(), base::Microseconds(60 * 1000000 / 30));
  std::optional<std::string> playlist = session->Playlist();
  ASSERT_TRUE(playlist);
  EXPECT_EQ(playlist->find("#EXT-X-KEY"), std::string::npos);
}

// A clear lead: the encrypted part gets its own init segment and key, after a
// discontinuity.
TEST(HlsSessionTest, ClearLead) {
  auto session = base::MakeRefCounted<HlsSession>(AudioConfig(), VideoConfig());
  Feed(*session, 0, 60, /*encrypted=*/false);
  Feed(*session, 60, 90, /*encrypted=*/true);
  ASSERT_TRUE(session->error().empty());
  EXPECT_EQ(session->segment_count(), 2u);
  std::string playlist = *session->Playlist();
  size_t discontinuity = playlist.find("#EXT-X-DISCONTINUITY");
  ASSERT_NE(discontinuity, std::string::npos);
  EXPECT_LT(playlist.find("seg-0.m4s"), discontinuity);
  EXPECT_LT(discontinuity, playlist.find("#EXT-X-MAP:URI=\"init-1.mp4\""));
  EXPECT_LT(discontinuity, playlist.find("#EXT-X-KEY:METHOD=SAMPLE-AES"));
}

TEST(HlsSessionTest, AudioOnly) {
  auto session = base::MakeRefCounted<HlsSession>(AudioConfig(), std::nullopt);
  std::vector<MseSample> audio;
  for (int i = 0; i < 48000 * 5 / 1024; ++i) {
    audio.push_back(AudioFrame(i));
  }
  session->Append(HlsSession::Track::kAudio, std::move(audio));
  session->EndOfStream(HlsSession::Track::kAudio);
  EXPECT_TRUE(session->error().empty());
  EXPECT_EQ(session->segment_count(), 3u);
  EXPECT_TRUE(session->ended());
}

TEST(HlsSessionTest, RefusesWhatAVPlayerCantPlay) {
  media::VideoDecoderConfig vp9 = VideoConfig();
  vp9.Initialize(media::VideoCodec::kVP9, media::VP9PROFILE_PROFILE0,
                 media::VideoDecoderConfig::AlphaMode::kIsOpaque,
                 media::VideoColorSpace::REC709(), media::kNoTransformation,
                 gfx::Size(320, 180), gfx::Rect(320, 180), gfx::Size(320, 180),
                 media::EmptyExtraData(), media::EncryptionScheme::kCbcs);
  auto session = base::MakeRefCounted<HlsSession>(std::nullopt, vp9);
  EXPECT_FALSE(session->error().empty());

  auto cenc = base::MakeRefCounted<HlsSession>(std::nullopt, VideoConfig());
  std::vector<MseSample> samples;
  samples.push_back(VideoFrame(0, true, false));
  samples.back().decrypt_config = media::DecryptConfig::CreateCencConfig(
      kKeyId, std::string(16, 'i'), {});
  cenc->Append(HlsSession::Track::kVideo, std::move(samples));
  EXPECT_NE(cenc->error().find("cbcs"), std::string::npos);
}

}  // namespace
}  // namespace compass::avf::mse
