// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/mse/sample_conversion.h"

#include "base/strings/string_number_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::avf::mse {
namespace {

std::vector<uint8_t> Hex(std::string_view hex) {
  std::vector<uint8_t> bytes;
  CHECK(base::HexStringToBytes(hex, &bytes));
  return bytes;
}

std::vector<uint8_t> Cat(std::initializer_list<std::vector<uint8_t>> parts) {
  std::vector<uint8_t> out;
  for (const auto& part : parts) {
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

std::vector<uint8_t> StartCode() {
  return {0, 0, 0, 1};
}
// From x264 and x265 (320x180, 8-bit 4:2:0).
const char kH264Sps[] = "6764000dacd941419f9f0110000003001000000303c0f1429960";
const char kH264Pps[] = "68ebe3cb22c0";
const char kHevcVps[] = "40010c01ffff01600000030090000003000003003c959809";
const char kHevcSps[] =
    "42010101600000030090000003000003003ca00a080b9f796566924caf01680800000300"
    "0800000300f040";
const char kHevcPps[] = "4401c172b46240";

TEST(SampleConversionTest, ClearAnnexB) {
  std::vector<uint8_t> idr = {0x65, 0x88, 0x84, 0x00, 0x21};
  std::vector<uint8_t> data =
      Cat({StartCode(), Hex(kH264Sps), StartCode(), Hex(kH264Pps), StartCode(), idr});
  std::vector<media::SubsampleEntry> subsamples;
  ParameterSets params;
  ASSERT_TRUE(
      AnnexBToLengthPrefixed(NalCodec::kH264, data, subsamples, params));
  EXPECT_EQ(data, Cat({{0, 0, 0, 5}, idr}));
  EXPECT_TRUE(subsamples.empty());
  ASSERT_EQ(params.sps.size(), 1u);
  EXPECT_EQ(params.sps[0], Hex(kH264Sps));
  ASSERT_EQ(params.pps.size(), 1u);
  EXPECT_EQ(params.pps[0], Hex(kH264Pps));

  // The same parameter sets again aren't added twice.
  data = Cat({StartCode(), Hex(kH264Sps), StartCode(), Hex(kH264Pps), StartCode(), idr});
  ASSERT_TRUE(
      AnnexBToLengthPrefixed(NalCodec::kH264, data, subsamples, params));
  EXPECT_EQ(params.sps.size(), 1u);
}

TEST(SampleConversionTest, NotAnnexB) {
  std::vector<uint8_t> data = {0, 0, 0, 5, 0x65, 1, 2, 3, 4};
  std::vector<media::SubsampleEntry> subsamples;
  ParameterSets params;
  EXPECT_FALSE(
      AnnexBToLengthPrefixed(NalCodec::kH264, data, subsamples, params));
}

// As Chromium's parser leaves an encrypted keyframe: parameter sets and the
// slice's start code and header clear, the slice data encrypted. A start
// code inside the encrypted bytes isn't one.
TEST(SampleConversionTest, EncryptedAnnexB) {
  std::vector<uint8_t> header = {0x65, 0x88, 0x84};  // 3 clear bytes
  std::vector<uint8_t> payload(32, 0xAB);
  payload[10] = 0;
  payload[11] = 0;
  payload[12] = 1;  // A fake start code, encrypted.
  std::vector<uint8_t> tail = {0x42, 0x43};  // Clear tail of the slice.
  std::vector<uint8_t> second = {0x41, 0x9A, 0x01};  // A second slice.
  std::vector<uint8_t> prefix =
      Cat({StartCode(), Hex(kH264Sps), StartCode(), Hex(kH264Pps), StartCode(), header});
  std::vector<uint8_t> data = Cat({prefix, payload, tail, StartCode(), second});
  std::vector<media::SubsampleEntry> subsamples = {
      {static_cast<uint32_t>(prefix.size()), 32},
      {static_cast<uint32_t>(tail.size() + 4 + second.size()), 0}};
  ParameterSets params;
  ASSERT_TRUE(
      AnnexBToLengthPrefixed(NalCodec::kH264, data, subsamples, params));

  std::vector<uint8_t> expected =
      Cat({{0, 0, 0, 37}, header, payload, tail, {0, 0, 0, 3}, second});
  EXPECT_EQ(data, expected);
  ASSERT_EQ(subsamples.size(), 2u);
  EXPECT_EQ(subsamples[0].clear_bytes, 4u + 3u);
  EXPECT_EQ(subsamples[0].cypher_bytes, 32u);
  EXPECT_EQ(subsamples[1].clear_bytes, 2u + 4u + 3u);
  EXPECT_EQ(subsamples[1].cypher_bytes, 0u);
  EXPECT_EQ(params.sps.size(), 1u);
}

TEST(SampleConversionTest, AvcC) {
  ParameterSets params;
  params.sps.push_back(Hex(kH264Sps));
  params.pps.push_back(Hex(kH264Pps));
  std::optional<std::vector<uint8_t>> avcc = MakeAvcC(params);
  ASSERT_TRUE(avcc);
  const std::vector<uint8_t>& box = *avcc;
  ASSERT_GT(box.size(), 16u);
  EXPECT_EQ(std::string(box.begin() + 4, box.begin() + 8), "avcC");
  EXPECT_EQ(box[8], 1);     // configurationVersion
  EXPECT_EQ(box[9], 0x64);  // High
  EXPECT_EQ(box[11], 0x0D);  // Level 1.3
  EXPECT_EQ(box[12], 0xFF);  // 4-byte lengths
  EXPECT_EQ(box[13], 0xE1);  // One SPS
  // High profile: chroma_format_idc 1, 8-bit.
  EXPECT_EQ(box[box.size() - 4], 0xFD);
  EXPECT_EQ(box[box.size() - 3], 0xF8);
  EXPECT_EQ(box[box.size() - 2], 0xF8);
  EXPECT_EQ(box.size(), box[3] + 0u);
}

TEST(SampleConversionTest, HvcC) {
  ParameterSets params;
  params.vps.push_back(Hex(kHevcVps));
  params.sps.push_back(Hex(kHevcSps));
  params.pps.push_back(Hex(kHevcPps));
  std::optional<std::vector<uint8_t>> hvcc = MakeHvcC(params);
  ASSERT_TRUE(hvcc);
  const std::vector<uint8_t>& box = *hvcc;
  EXPECT_EQ(std::string(box.begin() + 4, box.begin() + 8), "hvcC");
  EXPECT_EQ(box[8], 1);
  // general_profile_space 0, tier 0, Main (1); compatibility 0x60000000.
  EXPECT_EQ(box[9], 0x01);
  EXPECT_EQ(box[10], 0x60);
  EXPECT_EQ(box[20], 0x3C);  // general_level_idc: 2.0
  EXPECT_EQ(box[24], 0xFD);  // chroma_format_idc 1
  EXPECT_EQ(box[25], 0xF8);  // 8-bit luma
  EXPECT_EQ(box[26], 0xF8);  // 8-bit chroma
  EXPECT_EQ(box[29], 0x0F);  // 1 temporal layer, nested, 4-byte lengths
  EXPECT_EQ(box[30], 3);     // Three arrays: VPS, SPS, PPS
  EXPECT_EQ(box[31], 0x80 | 32);
}

TEST(SampleConversionTest, NeedsParameterSets) {
  ParameterSets params;
  params.sps.push_back(Hex(kH264Sps));
  EXPECT_FALSE(MakeAvcC(params));
  EXPECT_FALSE(MakeHvcC(params));
}

TEST(SampleConversionTest, Adts) {
  // AAC-LC, 48 kHz, stereo; protection absent.
  std::vector<uint8_t> frame = {0xFF, 0xF1, 0x4C, 0x80, 0x01, 0x7F, 0xFC,
                                0x21, 0x10, 0x04};
  std::optional<std::vector<uint8_t>> config =
      AudioSpecificConfigFromAdts(frame);
  ASSERT_TRUE(config);
  EXPECT_EQ(*config, (std::vector<uint8_t>{0x11, 0x90}));

  std::vector<media::SubsampleEntry> subsamples = {{7, 3}};
  ASSERT_TRUE(StripAdts(frame, subsamples));
  EXPECT_EQ(frame, (std::vector<uint8_t>{0x21, 0x10, 0x04}));
  EXPECT_EQ(subsamples[0].clear_bytes, 0u);
  EXPECT_EQ(subsamples[0].cypher_bytes, 3u);

  std::vector<uint8_t> raw = {0x21, 0x10, 0x04};
  std::vector<media::SubsampleEntry> none;
  EXPECT_FALSE(StripAdts(raw, none));
}

}  // namespace
}  // namespace compass::avf::mse
