// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_OS_CRYPT_LEGACY_KEYCHAIN_PASSWORD_H_
#define COMPASS_COMPONENTS_OS_CRYPT_LEGACY_KEYCHAIN_PASSWORD_H_

#include <optional>
#include <string>

namespace crypto::apple {
class KeychainV2;
}

namespace compass {

// Compass keeps the key that encrypts saved passwords and cookies in the
// Keychain item "Compass Safe Storage" (account "Compass"). Builds before
// 2026-11-01 used upstream's name for unbranded builds, "Chromium Safe
// Storage" (account "Chromium").
inline constexpr char kSafeStorageServiceName[] = "Compass Safe Storage";
inline constexpr char kSafeStorageAccountName[] = "Compass";
inline constexpr char kLegacySafeStorageServiceName[] =
    "Chromium Safe Storage";
inline constexpr char kLegacySafeStorageAccountName[] = "Chromium";

// Called by KeychainPassword::GetPassword() when |service_name| /
// |account_name| has no item (see
// patches/components-os_crypt-common-keychain_password_mac.mm.patch).
// Returns:
// - nullopt if this isn't Compass's item or there is no legacy item, so the
//   caller makes a new key;
// - the legacy item's password, after copying it to the new item (the
//   legacy item stays, for older builds); if the copy fails, the password
//   is still returned and the next launch tries again;
// - an empty string if the legacy item exists but can't be read (e.g. the
//   user denied access), so the caller doesn't make a new key that would
//   orphan the data encrypted with the old one.
std::optional<std::string> MoveLegacyKeychainPassword(
    crypto::apple::KeychainV2& keychain,
    const std::string& service_name,
    const std::string& account_name);

}  // namespace compass

#endif  // COMPASS_COMPONENTS_OS_CRYPT_LEGACY_KEYCHAIN_PASSWORD_H_
