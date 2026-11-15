// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/chromium_crypto.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include "base/apple/bridging.h"
#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/strings/sys_string_conversions.h"

namespace compass::browser_import {

std::optional<std::string> ReadKeychainPassword(std::string_view service) {
  NSDictionary* query = @{
    (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
    (__bridge id)kSecAttrService : base::SysUTF8ToNSString(std::string(service)),
    (__bridge id)kSecReturnData : @YES,
    (__bridge id)kSecMatchLimit : (__bridge id)kSecMatchLimitOne,
  };
  CFTypeRef result = nullptr;
  OSStatus status =
      SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
  base::apple::ScopedCFTypeRef<CFTypeRef> scoped_result(result);
  if (status != errSecSuccess || !result) {
    return std::nullopt;
  }
  NSData* data = base::apple::CFToNSPtrCast(
      base::apple::CFCastStrict<CFDataRef>(scoped_result.get()));
  return std::string(static_cast<const char*>(data.bytes), data.length);
}

}  // namespace compass::browser_import
