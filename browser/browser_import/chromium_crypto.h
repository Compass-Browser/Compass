// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_CHROMIUM_CRYPTO_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_CHROMIUM_CRYPTO_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/values.h"
#include "compass/browser/browser_import/chromium_reader.h"
#include "compass/browser/browser_import/source.h"

// Decrypting the passwords another Chromium-based browser saved
// (docs/design/browser-import.md, "Passwords"):
//
// - macOS: "v10", AES-128-CBC with a key from the browser's Keychain item
//   ("Chrome Safe Storage", ...), as Arc (arc::ChromiumMacCipher). macOS
//   asks the user whether Compass may read it.
// - Windows: "v10", AES-256-GCM with the key in Local State
//   (os_crypt.encrypted_key), which DPAPI protects for this Windows user.
//   "v20" (Chrome's app-bound encryption) only Chrome itself can read.
// - Linux: "v10", AES-128-CBC with Chromium's fixed key ("peanuts"). "v11",
//   whose key is in the desktop's keyring, isn't read.
namespace compass::browser_import {

// The browser's Keychain item on macOS ("Chrome Safe Storage").
std::string_view KeychainService(Browser browser);

// A decryptor for |browser|'s saved passwords, whose User Data is
// |user_data|; a null callback if its key can't be had. Blocking; on macOS
// the system may ask the user first.
PasswordDecryptor ChromiumPasswordDecryptor(Browser browser,
                                            const base::FilePath& user_data);

// Tests: the key the decryptor uses instead of the system's (macOS: the
// Keychain password; Windows: the AES-256 key). nullopt: the system's.
void SetChromiumKeyForTesting(std::optional<std::string> key);

// Windows' scheme, for tests: "v10" + 12-byte nonce + AES-256-GCM
// ciphertext and tag.
std::optional<std::string> DecryptWindowsValue(std::string_view key,
                                               std::string_view value);
std::string EncryptWindowsValueForTesting(std::string_view key,
                                          std::string_view plaintext);

#if BUILDFLAG(IS_WIN)
// The AES key Local State keeps, unwrapped with DPAPI ("DPAPI" prefix).
std::optional<std::string> UnwrapWindowsKey(const base::DictValue& local_state);
// The inverse, for tests: |key| wrapped with DPAPI for this user, as the
// os_crypt.encrypted_key value.
std::string WrapWindowsKeyForTesting(std::string_view key);
#endif

#if BUILDFLAG(IS_MAC)
// The Keychain item |service|'s password; nullopt if there's none or the
// user didn't allow Compass to read it. Blocking.
std::optional<std::string> ReadKeychainPassword(std::string_view service);
#endif

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_CHROMIUM_CRYPTO_H_
