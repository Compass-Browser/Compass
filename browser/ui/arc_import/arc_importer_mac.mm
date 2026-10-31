// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/arc_import/arc_importer.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include "base/apple/bridging.h"
#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/no_destructor.h"

namespace compass::arc {

namespace {

std::optional<std::optional<std::string>>& PasswordForTesting() {
  static base::NoDestructor<std::optional<std::optional<std::string>>> value;
  return *value;
}

}  // namespace

void SetArcKeychainPasswordForTesting(std::optional<std::string> password) {
  PasswordForTesting() = std::move(password);
}

std::optional<std::string> ReadArcKeychainPassword() {
  if (PasswordForTesting()) {
    return *PasswordForTesting();
  }
  NSDictionary* query = @{
    (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
    (__bridge id)kSecAttrService : @"Arc Safe Storage",
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

namespace {

constexpr NSString* kArcBundleId = @"company.thebrowser.Browser";


std::optional<bool>& RunningForTesting() {
  static std::optional<bool> running;
  return running;
}

}  // namespace

void SetArcRunningForTesting(std::optional<bool> running) {
  RunningForTesting() = running;
}

bool IsArcRunning() {
  if (RunningForTesting()) {
    return *RunningForTesting();
  }
  return [NSRunningApplication
             runningApplicationsWithBundleIdentifier:kArcBundleId]
             .count > 0;
}

bool QuitArc() {
  bool asked = false;
  for (NSRunningApplication* app in [NSRunningApplication
           runningApplicationsWithBundleIdentifier:kArcBundleId]) {
    asked |= [app terminate];
  }
  return asked;
}

}  // namespace compass::arc
