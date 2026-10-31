// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ARC_IMPORT_ARC_CRYPTO_H_
#define COMPASS_BROWSER_ARC_IMPORT_ARC_CRYPTO_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace compass::arc {

// Chromium's data encryption on macOS (os_crypt "v10"), as Arc uses it with
// its own Keychain password ("Arc Safe Storage"): AES-128-CBC with a key
// derived from the password by PBKDF2-HMAC-SHA1 ("saltysalt", 1003 rounds)
// and an IV of 16 spaces, the ciphertext prefixed with "v10".
class ChromiumMacCipher {
 public:
  static constexpr std::string_view kPrefix = "v10";

  explicit ChromiumMacCipher(std::string_view keychain_password);
  ChromiumMacCipher(const ChromiumMacCipher&);
  ChromiumMacCipher& operator=(const ChromiumMacCipher&);
  ~ChromiumMacCipher();

  // Whether |data| looks encrypted this way.
  static bool IsEncrypted(std::string_view data);

  // The plaintext, or nullopt if |ciphertext| isn't "v10" data of this key.
  std::optional<std::string> Decrypt(std::string_view ciphertext) const;
  std::string Encrypt(std::string_view plaintext) const;

 private:
  std::array<uint8_t, 16> key_;
};

}  // namespace compass::arc

#endif  // COMPASS_BROWSER_ARC_IMPORT_ARC_CRYPTO_H_
