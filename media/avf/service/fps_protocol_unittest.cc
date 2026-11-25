// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/fps_protocol.h"

#include "base/strings/string_number_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::avf {
namespace {

void U32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift : {24, 16, 8, 0}) {
    out.push_back((value >> shift) & 0xFF);
  }
}

void Append(std::vector<uint8_t>& out, const std::vector<uint8_t>& bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

std::vector<uint8_t> KeyId() {
  return {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
}

std::string KeyIdString() {
  std::vector<uint8_t> key_id = KeyId();
  return std::string(key_id.begin(), key_id.end());
}

// A key list as Apple's hls.js writes it: one key, an asset ID, no SSC, one
// version.
std::vector<uint8_t> KeyList() {
  std::vector<uint8_t> out;
  U32(out, 1);
  Append(out, KeyId());
  U32(out, 3);
  Append(out, {'a', 'b', 'c'});
  U32(out, 0);
  U32(out, 1);
  U32(out, 1);
  return out;
}

TEST(FpsProtocolTest, InitData) {
  std::vector<uint8_t> data;
  U32(data, 0x63626373);  // 'cbcs'
  U32(data, 1 << 24);
  Append(data, KeyList());

  std::vector<uint8_t> pssh;
  U32(pssh, 8 + 4 + 16 + 4 + data.size());
  U32(pssh, 0x70737368);
  U32(pssh, 0);  // version 0
  Append(pssh, {0x94, 0xCE, 0x86, 0xFB, 0x07, 0xFF, 0x4F, 0x43, 0xAD, 0xB8,
                0x93, 0xD2, 0xFA, 0x96, 0x8C, 0xA2});
  U32(pssh, data.size());
  Append(pssh, data);

  std::optional<FpsInitData> init = ParseFpsInitData(pssh);
  ASSERT_TRUE(init);
  EXPECT_EQ(init->format, FpsFormat::kBinary);
  const std::vector<FpsKeyRequest>* keys = &init->keys;
  ASSERT_EQ(keys->size(), 1u);
  EXPECT_EQ((*keys)[0].key_id, KeyIdString());
  EXPECT_EQ((*keys)[0].asset_id, (std::vector<uint8_t>{'a', 'b', 'c'}));

  // Another system's pssh isn't Safari's protocol.
  pssh[12] = 0xED;
  EXPECT_FALSE(ParseFpsInitData(pssh));
}

// Safari's format, as Apple TV+'s hls.js sends it (captured 2026-11-25).
TEST(FpsProtocolTest, WebKitInitData) {
  std::vector<uint8_t> pssh;
  ASSERT_TRUE(base::HexStringToBytes(
      "00000068707373680000000094ce86fb07ff4f43adb893d2fa968ca200000048000000"
      "486670736400000010667073690000000063626373000000306670736b0000001c666b"
      "726900000000000000000000000000000000000000030000000c666b766c00000001",
      &pssh));
  std::optional<FpsInitData> init = ParseFpsInitData(pssh);
  ASSERT_TRUE(init);
  EXPECT_EQ(init->format, FpsFormat::kWebKit);
  ASSERT_EQ(init->keys.size(), 1u);
  EXPECT_EQ(init->keys[0].key_id, std::string(15, '\0') + '\x03');
  EXPECT_TRUE(init->keys[0].asset_id.empty());
  EXPECT_EQ(init->keys[0].versions, (std::vector<uint32_t>{1}));
}

TEST(FpsProtocolTest, WebKitMessages) {
  std::vector<uint8_t> message = MakeFpsJsonMessage(KeyIdString(), {1, 2, 3});
  EXPECT_EQ(std::string(message.begin(), message.end()),
            "[{\"keyID\":\"AQIDBAUGBwgJCgsMDQ4PEA==\",\"payload\":\"AQID\"}]");
  // The page's update has the same shape, with the CKC.
  std::optional<std::vector<FpsLicense>> licenses =
      ParseFpsJsonLicenses(message);
  ASSERT_TRUE(licenses);
  ASSERT_EQ(licenses->size(), 1u);
  EXPECT_EQ((*licenses)[0].key_id, KeyIdString());
  EXPECT_EQ((*licenses)[0].ckc, (std::vector<uint8_t>{1, 2, 3}));
  std::vector<uint8_t> raw = {0xC0, 0xC1};
  EXPECT_FALSE(ParseFpsJsonLicenses(raw));
}

TEST(FpsProtocolTest, Messages) {
  std::vector<uint8_t> kreq;
  U32(kreq, 0x6B726571);  // 'kreq'
  Append(kreq, KeyList());
  std::optional<std::vector<FpsKeyRequest>> requests =
      ParseFpsKeyRequestMessage(kreq);
  ASSERT_TRUE(requests);
  EXPECT_EQ(requests->size(), 1u);
  EXPECT_FALSE(ParseFpsLicenseMessage(kreq));

  std::vector<uint8_t> ckcs;
  U32(ckcs, 0x636B6373);  // 'ckcs'
  U32(ckcs, 1);
  Append(ckcs, KeyId());
  U32(ckcs, 3600);
  U32(ckcs, 2);
  Append(ckcs, {0xC0, 0xC1});
  std::optional<std::vector<FpsLicense>> licenses =
      ParseFpsLicenseMessage(ckcs);
  ASSERT_TRUE(licenses);
  ASSERT_EQ(licenses->size(), 1u);
  EXPECT_EQ((*licenses)[0].expiry_seconds, 3600u);
  EXPECT_EQ((*licenses)[0].ckc, (std::vector<uint8_t>{0xC0, 0xC1}));

  ckcs.pop_back();  // Truncated.
  EXPECT_FALSE(ParseFpsLicenseMessage(ckcs));

  std::vector<uint8_t> spcs = MakeFpsSpcMessage(KeyIdString(), {0x5A});
  std::vector<uint8_t> expected;
  U32(expected, 0x73706373);  // 'spcs'
  U32(expected, 1);
  Append(expected, KeyId());
  U32(expected, 1);
  expected.push_back(0x5A);
  EXPECT_EQ(spcs, expected);
}

}  // namespace
}  // namespace compass::avf
