// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_SERVICE_MSE_SAMPLE_CONVERSION_H_
#define COMPASS_MEDIA_AVF_SERVICE_MSE_SAMPLE_CONVERSION_H_

#include <stdint.h>

#include <optional>
#include <vector>

#include "media/base/subsample_entry.h"

namespace compass::avf::mse {

// Undoes what Chromium's MP4 parser (media/formats/mp4) does to samples, so
// they can go back into MP4 for AVPlayer: H.264 and HEVC come out of it in
// Annex B with parameter sets before keyframes, AAC with an ADTS header. The
// subsamples (clear and encrypted byte counts) follow every change; encrypted
// bytes are never touched.

enum class NalCodec { kH264, kHevc };

// Parameter sets seen in a stream, in the order first seen, without
// duplicates.
struct ParameterSets {
  std::vector<std::vector<uint8_t>> vps;  // HEVC only.
  std::vector<std::vector<uint8_t>> sps;
  std::vector<std::vector<uint8_t>> pps;

  bool empty() const { return sps.empty() || pps.empty(); }
  bool operator==(const ParameterSets&) const = default;
};

// Annex B to NAL units each after a 4-byte length, leaving out parameter
// sets (they go in `params`, for the avcC/hvcC). Start codes are only looked
// for in clear bytes, so encrypted payloads can't fake one. False if the
// sample doesn't start with a start code.
bool AnnexBToLengthPrefixed(NalCodec codec,
                            std::vector<uint8_t>& data,
                            std::vector<media::SubsampleEntry>& subsamples,
                            ParameterSets& params);

// The 'avcC' and 'hvcC' boxes (complete, with their headers). Null if the
// parameter sets can't be parsed.
std::optional<std::vector<uint8_t>> MakeAvcC(const ParameterSets& params);
std::optional<std::vector<uint8_t>> MakeHvcC(const ParameterSets& params);

// Removes an ADTS header from an AAC frame. Returns false if there is none.
bool StripAdts(std::vector<uint8_t>& data,
               std::vector<media::SubsampleEntry>& subsamples);

// The AudioSpecificConfig an ADTS header describes, for when the stream's
// config has none.
std::optional<std::vector<uint8_t>> AudioSpecificConfigFromAdts(
    const std::vector<uint8_t>& frame);

}  // namespace compass::avf::mse

#endif  // COMPASS_MEDIA_AVF_SERVICE_MSE_SAMPLE_CONVERSION_H_
