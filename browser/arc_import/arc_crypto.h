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
#include <vector>

namespace compass::arc {

// Chromium's prefix for values encrypted with an app-bound key (Windows,
// "v20"): only the browser that wrote them, through its elevation service,
// can read them.
inline constexpr std::string_view kAppBoundPrefix = "v20";

// Chromium's data encryption on macOS (os_crypt "v10"), as Arc uses it with
// its own Keychain password ("Arc Safe Storage"): AES-128-CBC with a key
// derived from the password by PBKDF2-HMAC-SHA1 ("saltysalt", 1003 rounds)
// and an IV of 16 spaces, the ciphertext prefixed with "v10".
class ChromiumMacCipher {
 public:
  static constexpr std::string_view kPrefix = "v10";

  // |iterations|: Chromium's macOS PBKDF2 rounds; Linux's "v10" scheme is the
  // same with the password "peanuts" and one round.
  explicit ChromiumMacCipher(std::string_view keychain_password,
                             uint32_t iterations = 1003);
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

// Chromium's data encryption on Windows (os_crypt "v10"), as Arc for Windows
// uses it: AES-256-GCM with the key Local State keeps DPAPI-wrapped
// (os_crypt.encrypted_key); a value is "v10", a 12-byte nonce, then the
// ciphertext and its tag. Values with kAppBoundPrefix aren't this scheme.
class ChromiumWindowsCipher {
 public:
  static constexpr std::string_view kPrefix = "v10";
  static constexpr size_t kKeySize = 32;
  static constexpr size_t kNonceSize = 12;

  // |key|: the unwrapped AES-256 key (kKeySize bytes).
  explicit ChromiumWindowsCipher(std::string_view key);
  ChromiumWindowsCipher(const ChromiumWindowsCipher&);
  ChromiumWindowsCipher& operator=(const ChromiumWindowsCipher&);
  ~ChromiumWindowsCipher();

  // The plaintext, or nullopt if |ciphertext| isn't "v10" data of this key.
  std::optional<std::string> Decrypt(std::string_view ciphertext) const;
  // With a random nonce. |key| must be kKeySize bytes.
  std::string Encrypt(std::string_view plaintext) const;

 private:
  std::vector<uint8_t> key_;
};

}  // namespace compass::arc

#endif  // COMPASS_BROWSER_ARC_IMPORT_ARC_CRYPTO_H_
