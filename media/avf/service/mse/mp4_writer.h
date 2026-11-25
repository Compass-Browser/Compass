// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_SERVICE_MSE_MP4_WRITER_H_
#define COMPASS_MEDIA_AVF_SERVICE_MSE_MP4_WRITER_H_

#include <stdint.h>

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "media/base/subsample_entry.h"

namespace compass::avf::mse {

// Writes fragmented MP4 (ISO/IEC 14496-12, with Common Encryption's 'cbcs'
// boxes from ISO/IEC 23001-7) for the HLS AVPlayer plays an MSE stream from
// (docs/design/fairplay.md "FairPlay for MSE").

constexpr uint32_t FourCC(std::string_view code) {
  return (static_cast<uint32_t>(code[0]) << 24) |
         (static_cast<uint32_t>(code[1]) << 16) |
         (static_cast<uint32_t>(code[2]) << 8) | static_cast<uint32_t>(code[3]);
}

// Big-endian box writing, with sizes filled in when a box ends.
class BoxWriter {
 public:
  BoxWriter();
  ~BoxWriter();

  // Returns the box's offset, for EndBox().
  size_t StartBox(uint32_t type);
  size_t StartFullBox(uint32_t type, uint8_t version, uint32_t flags);
  void EndBox(size_t offset);

  void U8(uint8_t value);
  void U16(uint16_t value);
  void U24(uint32_t value);
  void U32(uint32_t value);
  void U64(uint64_t value);
  void Bytes(const std::vector<uint8_t>& bytes);
  void Bytes(const uint8_t* bytes, size_t size);
  void Zeros(size_t count);
  // Overwrites a U32 written earlier.
  void PatchU32(size_t offset, uint32_t value);

  size_t size() const { return buffer_.size(); }
  std::vector<uint8_t> Take() { return std::move(buffer_); }

 private:
  std::vector<uint8_t> buffer_;
};

// 'cbcs' protection of a track (tenc).
struct Mp4Encryption {
  std::array<uint8_t, 16> key_id = {};
  // The IV every sample uses; empty when samples carry their own (16 bytes).
  std::vector<uint8_t> constant_iv;
  uint8_t crypt_byte_block = 0;
  uint8_t skip_byte_block = 0;
};

struct Mp4Track {
  enum class Kind { kVideo, kAudio };

  Kind kind = Kind::kVideo;
  uint32_t track_id = 1;
  uint32_t timescale = 90000;
  // 'avc1', 'hvc1' or 'mp4a'; written as 'encv'/'enca' when encrypted.
  uint32_t format = 0;
  // The complete codec configuration box: 'avcC', 'hvcC' or 'esds'.
  std::vector<uint8_t> config_box;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t channels = 2;
  uint32_t sample_rate = 48000;
  std::optional<Mp4Encryption> encryption;
};

// ftyp + moov for the tracks.
std::vector<uint8_t> WriteInitSegment(const std::vector<Mp4Track>& tracks);

struct Mp4Sample {
  std::vector<uint8_t> data;
  uint32_t duration = 0;          // In the track's timescale.
  int32_t composition_offset = 0;  // PTS - DTS; may be negative.
  bool sync = false;
  // Encrypted samples: the IV when the track has none constant, and the
  // subsamples (none: the whole sample follows the pattern).
  std::vector<uint8_t> iv;
  std::vector<media::SubsampleEntry> subsamples;
};

struct Mp4Fragment {
  uint32_t track_id = 1;
  uint64_t base_decode_time = 0;
  bool encrypted = false;
  // 0 when the track's IV is constant, else 16.
  uint8_t per_sample_iv_size = 0;
  std::vector<Mp4Sample> samples;
};

// moof + mdat: one fragment per track, the samples in one mdat.
std::vector<uint8_t> WriteMediaSegment(
    uint32_t sequence_number,
    const std::vector<Mp4Fragment>& fragments);

// esds for AAC from its AudioSpecificConfig.
std::vector<uint8_t> MakeEsds(const std::vector<uint8_t>& audio_specific_config);

}  // namespace compass::avf::mse

#endif  // COMPASS_MEDIA_AVF_SERVICE_MSE_MP4_WRITER_H_
