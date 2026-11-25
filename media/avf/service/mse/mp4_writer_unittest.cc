// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/mse/mp4_writer.h"

#include <memory>

#include "base/containers/span.h"
#include "media/base/media_util.h"
#include "media/formats/mp4/box_definitions.h"
#include "media/formats/mp4/box_reader.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::avf::mse {
namespace {

using media::mp4::BoxReader;
using media::mp4::ParseResult;

// The box at `offset` in `data`, parsed with Chromium's MP4 parser.
template <typename T>
bool ParseBoxAt(const std::vector<uint8_t>& data, size_t offset, T* box) {
  media::NullMediaLog log;
  std::unique_ptr<BoxReader> reader;
  if (BoxReader::ReadTopLevelBox(base::span(data).subspan(offset), &log,
                                 &reader) != ParseResult::kOk) {
    return false;
  }
  return box->Parse(reader.get());
}

uint32_t BoxSize(const std::vector<uint8_t>& data, size_t offset) {
  return (data[offset] << 24) | (data[offset + 1] << 16) |
         (data[offset + 2] << 8) | data[offset + 3];
}

std::vector<Mp4Track> Tracks() {
  Mp4Track video;
  video.kind = Mp4Track::Kind::kVideo;
  video.track_id = 1;
  video.format = FourCC("avc1");
  // avcC from x264 (High, 320x180).
  video.config_box = {0,    0,    0,    0x29, 'a',  'v',  'c',  'C',  1,
                      0x64, 0,    0x0d, 0xff, 0xe1, 0,    0x1a, 0x67, 0x64,
                      0,    0x0d, 0xac, 0xd9, 0x41, 0x41, 0x9f, 0x9f, 0x01,
                      0x10, 0,    0,    0x03, 0,    0x10, 0,    0,    0x03,
                      0x03, 0xc0, 0xf1, 0x42, 0x99, 0x60, 1,    0,    6,
                      0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0, 0xfd, 0xf8, 0xf8,
                      0};
  video.config_box[3] = static_cast<uint8_t>(video.config_box.size());
  video.width = 320;
  video.height = 180;
  Mp4Encryption encryption;
  encryption.key_id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  encryption.constant_iv.assign(16, 0x77);
  encryption.crypt_byte_block = 1;
  encryption.skip_byte_block = 9;
  video.encryption = encryption;

  Mp4Track audio;
  audio.kind = Mp4Track::Kind::kAudio;
  audio.track_id = 2;
  audio.timescale = 48000;
  audio.format = FourCC("mp4a");
  audio.config_box = MakeEsds({0x11, 0x90});
  audio.channels = 2;
  audio.sample_rate = 48000;
  return {video, audio};
}

TEST(Mp4WriterTest, InitSegment) {
  std::vector<uint8_t> init = WriteInitSegment(Tracks());
  uint32_t ftyp_size = BoxSize(init, 0);
  EXPECT_EQ(std::string(init.begin() + 4, init.begin() + 8), "ftyp");

  media::mp4::Movie moov;
  ASSERT_TRUE(ParseBoxAt(init, ftyp_size, &moov));
  EXPECT_EQ(ftyp_size + BoxSize(init, ftyp_size), init.size());
  // mvex, which MSE parsers require: a trex per track.
  EXPECT_EQ(moov.extends.tracks.size(), 2u);
  ASSERT_EQ(moov.tracks.size(), 2u);

  const auto& video =
      moov.tracks[0].media.information.sample_table.description;
  ASSERT_EQ(video.video_entries.size(), 1u);
  const auto& entry = video.video_entries[0];
  EXPECT_EQ(entry.width, 320);
  EXPECT_EQ(entry.height, 180);
  EXPECT_EQ(entry.sinf.format.format, media::mp4::FOURCC_AVC1);
  EXPECT_EQ(entry.sinf.type.type, media::mp4::FOURCC_CBCS);
  const auto& tenc = entry.sinf.info.track_encryption;
  EXPECT_TRUE(tenc.is_encrypted);
  EXPECT_EQ(tenc.default_iv_size, 0);
  EXPECT_EQ(tenc.default_constant_iv_size, 16);
  EXPECT_EQ(tenc.default_crypt_byte_block, 1);
  EXPECT_EQ(tenc.default_skip_byte_block, 9);
  EXPECT_EQ(tenc.default_kid[15], 16);

  const auto& audio =
      moov.tracks[1].media.information.sample_table.description;
  ASSERT_EQ(audio.audio_entries.size(), 1u);
  EXPECT_EQ(audio.audio_entries[0].format, media::mp4::FOURCC_MP4A);
  EXPECT_EQ(audio.audio_entries[0].channelcount, 2);
  EXPECT_EQ(audio.audio_entries[0].samplerate, 48000u);
}

TEST(Mp4WriterTest, MediaSegment) {
  Mp4Fragment video;
  video.track_id = 1;
  video.base_decode_time = 90000;
  video.encrypted = true;
  for (int i = 0; i < 3; ++i) {
    Mp4Sample sample;
    sample.data.assign(100 + i, static_cast<uint8_t>(i));
    sample.duration = 3000;
    sample.composition_offset = i == 1 ? -3000 : 3000;
    sample.sync = i == 0;
    sample.subsamples = {{20, static_cast<uint32_t>(80 + i)}};
    video.samples.push_back(std::move(sample));
  }
  Mp4Fragment audio;
  audio.track_id = 2;
  audio.base_decode_time = 48000;
  for (int i = 0; i < 2; ++i) {
    Mp4Sample sample;
    sample.data.assign(10, 0xAA);
    sample.duration = 1024;
    sample.sync = true;
    audio.samples.push_back(std::move(sample));
  }

  std::vector<uint8_t> segment = WriteMediaSegment(7, {video, audio});
  media::mp4::MovieFragment moof;
  ASSERT_TRUE(ParseBoxAt(segment, 0, &moof));
  EXPECT_EQ(moof.header.sequence_number, 7u);
  ASSERT_EQ(moof.tracks.size(), 2u);

  const auto& traf = moof.tracks[0];
  EXPECT_EQ(traf.decode_time.decode_time, 90000u);
  ASSERT_EQ(traf.runs.size(), 1u);
  const auto& trun = traf.runs[0];
  EXPECT_EQ(trun.sample_count, 3u);
  EXPECT_EQ(trun.sample_sizes[2], 102u);
  EXPECT_EQ(trun.sample_composition_time_offsets[1], -3000);
  // The first sample starts right after the mdat header.
  uint32_t moof_size = BoxSize(segment, 0);
  EXPECT_EQ(trun.data_offset, moof_size + 8);
  EXPECT_EQ(segment[trun.data_offset + 100], 1);  // Second sample's bytes.
  EXPECT_EQ(traf.auxiliary_size.sample_count, 3u);
  EXPECT_EQ(traf.auxiliary_offset.offsets.size(), 1u);
  EXPECT_TRUE(traf.sample_encryption.use_subsample_encryption);

  const auto& audio_run = moof.tracks[1].runs[0];
  EXPECT_EQ(audio_run.data_offset, moof_size + 8 + 100 + 101 + 102);
  std::string mdat(segment.begin() + moof_size + 4,
                   segment.begin() + moof_size + 8);
  EXPECT_EQ(mdat, "mdat");
  EXPECT_EQ(segment.size(), moof_size + 8 + 303 + 20u);
}

}  // namespace
}  // namespace compass::avf::mse
