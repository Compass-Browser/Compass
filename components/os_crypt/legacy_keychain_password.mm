// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/os_crypt/legacy_keychain_password.h"

#include <Security/Security.h>

#include "base/containers/span.h"
#include "base/logging.h"
#include "base/apple/osstatus_logging.h"
#include "crypto/apple/keychain_v2.h"

namespace compass {

std::optional<std::string> MoveLegacyKeychainPassword(
    crypto::apple::KeychainV2& keychain,
    const std::string& service_name,
    const std::string& account_name) {
  if (service_name != kSafeStorageServiceName ||
      account_name != kSafeStorageAccountName) {
    return std::nullopt;
  }
  auto legacy = keychain.FindGenericPassword(kLegacySafeStorageServiceName,
                                             kLegacySafeStorageAccountName);
  if (!legacy.has_value()) {
    if (legacy.error() == errSecItemNotFound) {
      return std::nullopt;
    }
    OSSTATUS_LOG(ERROR, legacy.error())
        << "Can't read \"" << kLegacySafeStorageServiceName << "\"";
    return std::string();
  }
  if (legacy->empty()) {
    return std::nullopt;
  }
  std::string password(legacy->begin(), legacy->end());
  const OSStatus added = keychain.AddGenericPassword(
      service_name, account_name, base::as_byte_span(password));
  if (added != noErr) {
    OSSTATUS_LOG(ERROR, added)
        << "Can't copy \"" << kLegacySafeStorageServiceName << "\" to \""
        << service_name << "\"; using it as it is";
  } else {
    LOG(WARNING) << "Moved the safe storage key from \""
                 << kLegacySafeStorageServiceName << "\" to \""
                 << service_name << "\"";
  }
  return password;
}

}  // namespace compass
