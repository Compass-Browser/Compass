// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/chain.h"

#include "base/base64.h"
#include "base/base64url.h"
#include "base/check.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "crypto/hash.h"
#include "crypto/kdf.h"
#include "crypto/random.h"
#include "third_party/boringssl/src/include/openssl/curve25519.h"

namespace compass::sync {
namespace {

std::array<uint8_t, 32> Derive(const Seed& seed, std::string_view info) {
  return crypto::kdf::Hkdf<32>(crypto::hash::kSha256, seed,
                               /*salt=*/base::span<const uint8_t>(),
                               base::as_byte_span(info));
}

struct KeyPair {
  PublicKey public_key{};
  std::array<uint8_t, 64> private_key{};
};

KeyPair ExpandKey(const AuthKey& key) {
  KeyPair pair;
  ED25519_keypair_from_seed(pair.public_key.data(), pair.private_key.data(),
                            key.data());
  return pair;
}

}  // namespace

Seed GenerateSeed() {
  return crypto::RandBytesAsArray<kSeedSize>();
}

AuthKey DeriveAuthKey(const Seed& seed, std::string_view profile_uuid) {
  if (profile_uuid.empty()) {
    return Derive(seed, kAuthInfo);
  }
  return Derive(seed, base::StrCat({kAuthInfo, ":", profile_uuid}));
}

std::string DerivePassphrase(const Seed& seed) {
  return base::Base64Encode(Derive(seed, kPassphraseInfo));
}

PublicKey GetPublicKey(const AuthKey& key) {
  return ExpandKey(key).public_key;
}

std::string GetChainId(const AuthKey& key) {
  return base::HexEncodeLower(GetPublicKey(key));
}

std::string MakeAccessToken(const AuthKey& key, base::Time now) {
  const KeyPair pair = ExpandKey(key);
  const std::string timestamp =
      base::NumberToString(now.InMillisecondsSinceUnixEpoch());
  std::array<uint8_t, 64> signature{};
  CHECK(ED25519_sign(signature.data(),
                     reinterpret_cast<const uint8_t*>(timestamp.data()),
                     timestamp.size(), pair.private_key.data()));
  const std::string plain = base::StrCat(
      {base::HexEncode(std::string_view(timestamp)), "|",
       base::HexEncode(signature), "|", base::HexEncode(pair.public_key)});
  std::string token;
  base::Base64UrlEncode(plain, base::Base64UrlEncodePolicy::INCLUDE_PADDING,
                        &token);
  return token;
}

std::vector<std::string> SeedToRecoveryWords(const Seed& seed) {
  return EntropyToRecoveryWords(seed);
}

std::optional<Seed> RecoveryCodeToSeed(std::string_view text) {
  return ParseRecoveryCode(text);
}

}  // namespace compass::sync
