// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/updates/update_hooks.h"

#import <Foundation/Foundation.h>

#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#include "base/strings/sys_string_conversions.h"

namespace compass::updates {

std::string CompassVersion() {
  // The outer app's Info.plist: the release tool stamps CFBundleVersion with
  // Compass's version (compass_version in GN). Without it, CFBundleVersion is
  // upstream's BUILD.PATCH, which isn't a Compass version.
  NSBundle* app = base::apple::OuterBundle();
  NSString* version = base::apple::ObjCCast<NSString>(
      [app objectForInfoDictionaryKey:@"CompassVersion"]);
  return version ? base::SysNSStringToUTF8(version) : std::string();
}

}  // namespace compass::updates
