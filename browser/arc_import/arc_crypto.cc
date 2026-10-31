// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/arc_crypto.h"

#include <vector>

#include "base/containers/span.h"
#include "crypto/aes_cbc.h"
#include "crypto/kdf.h"
#include "crypto/subtle_passkey.h"

namespace compass::arc {

// Allowed by crypto/subtle_passkey.h (patch).
crypto::SubtlePassKey MakeCryptoPassKey() {
  return crypto::SubtlePassKey();
}

namespace {

constexpr char kSalt[] = "saltysalt";
constexpr uint32_t kIterations = 1003;
constexpr std::array<uint8_t, 16> kIv = {' ', ' ', ' ', ' ', ' ', ' ',
                                         ' ', ' ', ' ', ' ', ' ', ' ',
                                         ' ', ' ', ' ', ' '};

}  // namespace

ChromiumMacCipher::ChromiumMacCipher(std::string_view keychain_password) {
  crypto::kdf::Pbkdf2HmacSha1({.iterations = kIterations},
                              base::as_byte_span(keychain_password),
                              base::as_byte_span(std::string_view(kSalt)),
                              key_, MakeCryptoPassKey());
}

ChromiumMacCipher::ChromiumMacCipher(const ChromiumMacCipher&) = default;
ChromiumMacCipher& ChromiumMacCipher::operator=(const ChromiumMacCipher&) =
    default;
ChromiumMacCipher::~ChromiumMacCipher() = default;

// static
bool ChromiumMacCipher::IsEncrypted(std::string_view data) {
  return data.starts_with(kPrefix);
}

std::optional<std::string> ChromiumMacCipher::Decrypt(
    std::string_view ciphertext) const {
  if (!IsEncrypted(ciphertext)) {
    return std::nullopt;
  }
  ciphertext.remove_prefix(kPrefix.size());
  std::optional<std::vector<uint8_t>> plaintext =
      crypto::aes_cbc::Decrypt(key_, kIv, base::as_byte_span(ciphertext));
  if (!plaintext) {
    return std::nullopt;
  }
  return std::string(plaintext->begin(), plaintext->end());
}

std::string ChromiumMacCipher::Encrypt(std::string_view plaintext) const {
  std::vector<uint8_t> ciphertext =
      crypto::aes_cbc::Encrypt(key_, kIv, base::as_byte_span(plaintext));
  return std::string(kPrefix) + std::string(ciphertext.begin(), ciphertext.end());
}

}  // namespace compass::arc
