// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_SERVICE_FPS_PROTOCOL_H_
#define COMPASS_MEDIA_AVF_SERVICE_FPS_PROTOCOL_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

namespace compass::avf {

// Safari's FairPlay protocol over EME, for pages that use it (Apple TV+'s
// hls.js; docs/design/fairplay.md "Safari's FairPlay protocol"). All
// big-endian:
//
//   init data ('cenc'): a 'pssh' box with FairPlay's system ID whose data is
//     u32 scheme, u32 version << 24 | flags, then the key list
//   key list: u32 count, then per key: key ID (16 bytes), u32 + asset ID,
//     u32 + SSC, u32 count + u32 versions
//   update 'kreq' (or 'okrk', offline) + key list: make SPCs for these keys
//   message 'spcs': u32 count, then per key: key ID, u32 + SPC
//   update 'ckcs' (or 'okcs'): u32 count, then per key: key ID, u32 expiry
//     (seconds), u32 + CKC

struct FpsKeyRequest {
  std::string key_id;  // 16 bytes.
  std::vector<uint8_t> asset_id;
  // The FairPlay protocol versions the page's server takes for this key.
  std::vector<uint32_t> versions;
};

// Which of the two the page speaks:
//   kBinary: the 'kreq'/'spcs'/'ckcs' messages above.
//   kWebKit: Safari's own (WebKit's CDMInstanceFairPlayStreamingAVFObjC,
//     Apple TV+'s hls.js "com.apple.fps.3_0"): the 'pssh' data is a box
//     'fpsd' holding 'fpsi' (version, scheme) and per key 'fpsk' with 'fkri'
//     (version, key ID), 'fkai' (asset ID), 'fkcx' (context), 'fkvl'
//     (versions); license requests and updates are JSON,
//     [{"keyID": base64, "payload": base64}], the payload an SPC or a CKC;
//     update("renew") asks for a renewal.
enum class FpsFormat { kBinary, kWebKit };

struct FpsInitData {
  FpsFormat format = FpsFormat::kBinary;
  std::vector<FpsKeyRequest> keys;
};

struct FpsLicense {
  std::string key_id;
  uint32_t expiry_seconds = 0;
  std::vector<uint8_t> ckc;
};

// The keys of a FairPlay 'pssh' box in 'cenc' init data; null if there's
// none (plain 'cenc', handled by key IDs).
std::optional<FpsInitData> ParseFpsInitData(
    const std::vector<uint8_t>& init_data);

// An update() of Safari's protocol: 'kreq' or 'ckcs'. Null if `message`
// isn't one.
std::optional<std::vector<FpsKeyRequest>> ParseFpsKeyRequestMessage(
    const std::vector<uint8_t>& message);
std::optional<std::vector<FpsLicense>> ParseFpsLicenseMessage(
    const std::vector<uint8_t>& message);

// The 'spcs' message for one key's SPC.
std::vector<uint8_t> MakeFpsSpcMessage(const std::string& key_id,
                                       const std::vector<uint8_t>& spc);

// Safari's JSON messages: the license request for one key's SPC, and the
// CKCs in an update (null if `message` isn't that JSON).
std::vector<uint8_t> MakeFpsJsonMessage(const std::string& key_id,
                                        const std::vector<uint8_t>& payload);
std::optional<std::vector<FpsLicense>> ParseFpsJsonLicenses(
    const std::vector<uint8_t>& message);

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_SERVICE_FPS_PROTOCOL_H_
