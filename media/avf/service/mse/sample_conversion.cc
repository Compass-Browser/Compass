// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include "compass/media/avf/service/mse/sample_conversion.h"

#include <algorithm>
#include <utility>

#include "compass/media/avf/service/mse/mp4_writer.h"

namespace compass::avf::mse {
namespace {

struct Region {
  size_t start;
  size_t end;
  bool clear;
};

// Clear and encrypted byte runs, merged, turned back into subsamples.
class RunBuilder {
 public:
  void Add(bool clear, size_t length) {
    if (!length) {
      return;
    }
    if (!runs_.empty() && runs_.back().first == clear) {
      runs_.back().second += length;
    } else {
      runs_.emplace_back(clear, length);
    }
  }

  std::vector<media::SubsampleEntry> Subsamples() const {
    std::vector<media::SubsampleEntry> out;
    media::SubsampleEntry current = {0, 0};
    for (const auto& [clear, length] : runs_) {
      if (clear) {
        if (current.cypher_bytes) {
          out.push_back(current);
          current = {0, 0};
        }
        current.clear_bytes += length;
      } else {
        current.cypher_bytes += length;
      }
    }
    if (current.clear_bytes || current.cypher_bytes) {
      out.push_back(current);
    }
    return out;
  }

 private:
  std::vector<std::pair<bool, size_t>> runs_;
};

std::optional<std::vector<Region>> RegionsOf(
    size_t size,
    const std::vector<media::SubsampleEntry>& subsamples) {
  std::vector<Region> regions;
  if (subsamples.empty()) {
    regions.push_back({0, size, true});
    return regions;
  }
  size_t offset = 0;
  for (const auto& subsample : subsamples) {
    if (subsample.clear_bytes) {
      regions.push_back({offset, offset + subsample.clear_bytes, true});
      offset += subsample.clear_bytes;
    }
    if (subsample.cypher_bytes) {
      regions.push_back({offset, offset + subsample.cypher_bytes, false});
      offset += subsample.cypher_bytes;
    }
  }
  if (offset > size) {
    return std::nullopt;
  }
  if (offset < size) {
    regions.push_back({offset, size, true});
  }
  return regions;
}

bool IsClear(const std::vector<Region>& regions, size_t start, size_t end) {
  for (const auto& region : regions) {
    if (region.start <= start && end <= region.end) {
      return region.clear;
    }
  }
  return false;
}

void AddUnique(std::vector<std::vector<uint8_t>>& list,
               std::vector<uint8_t> nal) {
  if (std::find(list.begin(), list.end(), nal) == list.end()) {
    list.push_back(std::move(nal));
  }
}

// RBSP of a NAL unit's payload: emulation prevention bytes removed.
std::vector<uint8_t> Unescape(const std::vector<uint8_t>& nal, size_t from) {
  std::vector<uint8_t> out;
  int zeros = 0;
  for (size_t i = from; i < nal.size(); ++i) {
    if (zeros >= 2 && nal[i] == 0x03) {
      zeros = 0;
      continue;
    }
    zeros = nal[i] == 0 ? zeros + 1 : 0;
    out.push_back(nal[i]);
  }
  return out;
}

class BitReader {
 public:
  explicit BitReader(std::vector<uint8_t> data) : data_(std::move(data)) {}

  bool Bits(int count, uint32_t* value) {
    uint32_t result = 0;
    for (int i = 0; i < count; ++i) {
      if (position_ >= data_.size() * 8) {
        return false;
      }
      result = (result << 1) |
               ((data_[position_ / 8] >> (7 - position_ % 8)) & 1);
      ++position_;
    }
    *value = result;
    return true;
  }

  bool Skip(int count) {
    uint32_t ignored;
    while (count > 0) {
      int step = std::min(count, 32);
      if (!Bits(step, &ignored)) {
        return false;
      }
      count -= step;
    }
    return true;
  }

  // Exp-Golomb.
  bool Ue(uint32_t* value) {
    int leading_zeros = 0;
    uint32_t bit = 0;
    while (true) {
      if (!Bits(1, &bit)) {
        return false;
      }
      if (bit) {
        break;
      }
      if (++leading_zeros > 31) {
        return false;
      }
    }
    uint32_t rest = 0;
    if (leading_zeros && !Bits(leading_zeros, &rest)) {
      return false;
    }
    *value = (1u << leading_zeros) - 1 + rest;
    return true;
  }

 private:
  const std::vector<uint8_t> data_;  // A parameter set's RBSP: small.
  size_t position_ = 0;
};

void WriteNalArray(BoxWriter& w,
                   uint8_t type,
                   const std::vector<std::vector<uint8_t>>& nals) {
  w.U8(0x80 | type);  // array_completeness = 1
  w.U16(static_cast<uint16_t>(nals.size()));
  for (const auto& nal : nals) {
    w.U16(static_cast<uint16_t>(nal.size()));
    w.Bytes(nal);
  }
}

}  // namespace

bool AnnexBToLengthPrefixed(NalCodec codec,
                            std::vector<uint8_t>& data,
                            std::vector<media::SubsampleEntry>& subsamples,
                            ParameterSets& params) {
  std::optional<std::vector<Region>> regions =
      RegionsOf(data.size(), subsamples);
  if (!regions) {
    return false;
  }

  // Start codes (begin, NAL begin), only in clear bytes.
  std::vector<std::pair<size_t, size_t>> starts;
  for (const auto& region : *regions) {
    if (!region.clear) {
      continue;
    }
    for (size_t i = region.start; i + 3 <= region.end; ++i) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        size_t begin = (i > region.start && data[i - 1] == 0) ? i - 1 : i;
        starts.emplace_back(begin, i + 3);
        i += 2;
      }
    }
  }
  if (starts.empty() || starts.front().first != 0) {
    return false;
  }

  std::vector<uint8_t> out;
  out.reserve(data.size());
  RunBuilder runs;
  for (size_t n = 0; n < starts.size(); ++n) {
    size_t begin = starts[n].second;
    size_t end = n + 1 < starts.size() ? starts[n + 1].first : data.size();
    // trailing_zero_8bits before the next start code.
    while (end > begin && data[end - 1] == 0 &&
           IsClear(*regions, end - 1, end)) {
      --end;
    }
    if (end <= begin) {
      continue;
    }
    uint8_t type = codec == NalCodec::kH264 ? (data[begin] & 0x1F)
                                            : ((data[begin] >> 1) & 0x3F);
    bool parameter_set = false;
    if (IsClear(*regions, begin, end)) {
      std::vector<uint8_t> nal(data.begin() + begin, data.begin() + end);
      if (codec == NalCodec::kH264) {
        if (type == 7) {
          AddUnique(params.sps, std::move(nal));
          parameter_set = true;
        } else if (type == 8) {
          AddUnique(params.pps, std::move(nal));
          parameter_set = true;
        }
      } else {
        if (type == 32) {
          AddUnique(params.vps, std::move(nal));
          parameter_set = true;
        } else if (type == 33) {
          AddUnique(params.sps, std::move(nal));
          parameter_set = true;
        } else if (type == 34) {
          AddUnique(params.pps, std::move(nal));
          parameter_set = true;
        }
      }
    }
    if (parameter_set) {
      continue;
    }

    uint32_t length = static_cast<uint32_t>(end - begin);
    out.push_back(length >> 24);
    out.push_back((length >> 16) & 0xFF);
    out.push_back((length >> 8) & 0xFF);
    out.push_back(length & 0xFF);
    runs.Add(true, 4);
    for (const auto& region : *regions) {
      size_t from = std::max(region.start, begin);
      size_t to = std::min(region.end, end);
      if (from < to) {
        out.insert(out.end(), data.begin() + from, data.begin() + to);
        runs.Add(region.clear, to - from);
      }
    }
  }

  bool encrypted = !subsamples.empty();
  data = std::move(out);
  subsamples = encrypted ? runs.Subsamples()
                         : std::vector<media::SubsampleEntry>();
  return true;
}

std::optional<std::vector<uint8_t>> MakeAvcC(const ParameterSets& params) {
  if (params.empty() || params.sps[0].size() < 4) {
    return std::nullopt;
  }
  const std::vector<uint8_t>& sps = params.sps[0];
  uint8_t profile = sps[1];
  BoxWriter w;
  size_t avcc = w.StartBox(FourCC("avcC"));
  w.U8(1);
  w.U8(profile);
  w.U8(sps[2]);
  w.U8(sps[3]);
  w.U8(0xFF);  // lengthSizeMinusOne = 3
  w.U8(static_cast<uint8_t>(0xE0 | params.sps.size()));
  for (const auto& nal : params.sps) {
    w.U16(static_cast<uint16_t>(nal.size()));
    w.Bytes(nal);
  }
  w.U8(static_cast<uint8_t>(params.pps.size()));
  for (const auto& nal : params.pps) {
    w.U16(static_cast<uint16_t>(nal.size()));
    w.Bytes(nal);
  }
  if (profile == 100 || profile == 110 || profile == 122 || profile == 144) {
    std::vector<uint8_t> rbsp = Unescape(sps, 1);
    BitReader r(rbsp);
    uint32_t id, chroma = 1, separate, luma = 0, chroma_depth = 0;
    if (!r.Skip(24) || !r.Ue(&id) || !r.Ue(&chroma) ||
        (chroma == 3 && !r.Bits(1, &separate)) || !r.Ue(&luma) ||
        !r.Ue(&chroma_depth)) {
      return std::nullopt;
    }
    w.U8(static_cast<uint8_t>(0xFC | (chroma & 3)));
    w.U8(static_cast<uint8_t>(0xF8 | (luma & 7)));
    w.U8(static_cast<uint8_t>(0xF8 | (chroma_depth & 7)));
    w.U8(0);  // numOfSequenceParameterSetExt
  }
  w.EndBox(avcc);
  return w.Take();
}

std::optional<std::vector<uint8_t>> MakeHvcC(const ParameterSets& params) {
  if (params.empty() || params.vps.empty() || params.sps[0].size() < 16) {
    return std::nullopt;
  }
  std::vector<uint8_t> rbsp = Unescape(params.sps[0], 2);
  if (rbsp.size() < 13) {
    return std::nullopt;
  }
  BitReader r(rbsp);
  uint32_t vps_id, max_sub_layers, nesting;
  r.Bits(4, &vps_id);
  r.Bits(3, &max_sub_layers);
  r.Bits(1, &nesting);
  r.Skip(96);  // general profile_tier_level: copied from rbsp[1..12] below
  std::vector<bool> profile_present(max_sub_layers), level_present(
                                                         max_sub_layers);
  for (uint32_t i = 0; i < max_sub_layers; ++i) {
    uint32_t bit;
    if (!r.Bits(1, &bit)) {
      return std::nullopt;
    }
    profile_present[i] = bit;
    if (!r.Bits(1, &bit)) {
      return std::nullopt;
    }
    level_present[i] = bit;
  }
  if (max_sub_layers > 0) {
    r.Skip(2 * (8 - max_sub_layers));
  }
  for (uint32_t i = 0; i < max_sub_layers; ++i) {
    if (profile_present[i]) {
      r.Skip(88);
    }
    if (level_present[i]) {
      r.Skip(8);
    }
  }
  uint32_t sps_id, chroma = 1, separate, width, height, window, luma = 0,
                   chroma_depth = 0, ignored;
  if (!r.Ue(&sps_id) || !r.Ue(&chroma) ||
      (chroma == 3 && !r.Bits(1, &separate)) || !r.Ue(&width) ||
      !r.Ue(&height) || !r.Bits(1, &window)) {
    return std::nullopt;
  }
  if (window) {
    for (int i = 0; i < 4; ++i) {
      if (!r.Ue(&ignored)) {
        return std::nullopt;
      }
    }
  }
  if (!r.Ue(&luma) || !r.Ue(&chroma_depth)) {
    return std::nullopt;
  }

  BoxWriter w;
  size_t hvcc = w.StartBox(FourCC("hvcC"));
  w.U8(1);
  // general_profile_space .. general_level_idc, as in the SPS.
  w.Bytes(rbsp.data() + 1, 12);
  w.U16(0xF000);  // min_spatial_segmentation_idc = 0
  w.U8(0xFC);     // parallelismType = 0
  w.U8(static_cast<uint8_t>(0xFC | (chroma & 3)));
  w.U8(static_cast<uint8_t>(0xF8 | (luma & 7)));
  w.U8(static_cast<uint8_t>(0xF8 | (chroma_depth & 7)));
  w.U16(0);  // avgFrameRate
  // constantFrameRate 0, numTemporalLayers, temporalIdNested,
  // lengthSizeMinusOne 3.
  w.U8(static_cast<uint8_t>((((max_sub_layers + 1) & 7) << 3) |
                            ((nesting & 1) << 2) | 3));
  w.U8(3);
  WriteNalArray(w, 32, params.vps);
  WriteNalArray(w, 33, params.sps);
  WriteNalArray(w, 34, params.pps);
  w.EndBox(hvcc);
  return w.Take();
}

bool StripAdts(std::vector<uint8_t>& data,
               std::vector<media::SubsampleEntry>& subsamples) {
  if (data.size() < 7 || data[0] != 0xFF || (data[1] & 0xF6) != 0xF0) {
    return false;
  }
  size_t header = (data[1] & 0x01) ? 7 : 9;
  if (data.size() < header) {
    return false;
  }
  if (!subsamples.empty()) {
    if (subsamples[0].clear_bytes < header) {
      return false;
    }
    subsamples[0].clear_bytes -= header;
  }
  data.erase(data.begin(), data.begin() + header);
  return true;
}

std::optional<std::vector<uint8_t>> AudioSpecificConfigFromAdts(
    const std::vector<uint8_t>& frame) {
  if (frame.size() < 7 || frame[0] != 0xFF || (frame[1] & 0xF6) != 0xF0) {
    return std::nullopt;
  }
  uint32_t object_type = ((frame[2] >> 6) & 0x3) + 1;
  uint32_t frequency = (frame[2] >> 2) & 0xF;
  uint32_t channels = ((frame[2] & 0x1) << 2) | ((frame[3] >> 6) & 0x3);
  uint16_t config = static_cast<uint16_t>((object_type << 11) |
                                          (frequency << 7) | (channels << 3));
  return std::vector<uint8_t>{static_cast<uint8_t>(config >> 8),
                              static_cast<uint8_t>(config & 0xFF)};
}

}  // namespace compass::avf::mse
