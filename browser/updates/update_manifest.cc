// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/updates/update_manifest.h"

#include <vector>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "base/version.h"
#include "crypto/hash.h"
#include "third_party/boringssl/src/include/openssl/curve25519.h"

namespace compass::updates {

GURL ChannelManifestUrl(std::string_view channel) {
  return GURL(std::string(kUpdateServer) + "/" + std::string(channel) +
              ".json");
}

std::optional<ManifestEntry> ParseChannelManifest(std::string_view json,
                                                  std::string_view platform) {
  std::optional<base::Value> root = base::JSONReader::Read(
      json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!root || !root->is_dict()) {
    return std::nullopt;
  }
  const base::DictValue* entry = root->GetDict().FindDict(platform);
  if (!entry) {
    return std::nullopt;
  }
  const std::string* version = entry->FindString("version");
  const std::string* chromium = entry->FindString("chromium");
  const std::string* url = entry->FindString("url");
  const std::string* sha256 = entry->FindString("sha256");
  const std::string* signature = entry->FindString("ed_signature");
  std::optional<double> size = entry->FindDouble("size");
  if (!version || !url || !sha256 || !signature || !size || *size <= 0) {
    return std::nullopt;
  }
  ManifestEntry out;
  out.version = *version;
  out.chromium = chromium ? *chromium : std::string();
  out.url = GURL(*url);
  out.size = static_cast<int64_t>(*size);
  out.sha256 = base::ToLowerASCII(*sha256);
  out.ed_signature = *signature;
  // Installers come from the update server only (either of its names).
  const GURL origin = out.url.DeprecatedGetOriginAsURL();
  if (!out.url.SchemeIs("https") ||
      (origin != GURL(kUpdateServer) && origin != GURL(kOldUpdateServer))) {
    return std::nullopt;
  }
  if (!base::Version(out.version).IsValid() || out.sha256.size() != 64) {
    return std::nullopt;
  }
  return out;
}

bool IsNewerVersion(std::string_view candidate, std::string_view running) {
  base::Version next(candidate);
  base::Version current(running);
  return next.IsValid() && current.IsValid() && next.CompareTo(current) > 0;
}

bool VerifyDownload(base::span<const uint8_t> data,
                    const ManifestEntry& entry,
                    std::string_view public_key_base64) {
  if (static_cast<int64_t>(data.size()) != entry.size) {
    return false;
  }
  std::array<uint8_t, crypto::hash::kSha256Size> digest =
      crypto::hash::Sha256(data);
  if (base::ToLowerASCII(base::HexEncode(digest)) != entry.sha256) {
    return false;
  }
  std::optional<std::vector<uint8_t>> key =
      base::Base64Decode(public_key_base64);
  std::optional<std::vector<uint8_t>> signature =
      base::Base64Decode(entry.ed_signature);
  if (!key || key->size() != 32 || !signature || signature->size() != 64) {
    return false;
  }
  return ED25519_verify(data.data(), data.size(), signature->data(),
                        key->data()) == 1;
}

}  // namespace compass::updates
