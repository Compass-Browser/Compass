// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/detect.h"

#import <AppKit/AppKit.h>

#include "base/strings/sys_string_conversions.h"

namespace compass::browser_import {

bool IsMacAppRunning(std::string_view bundle_id) {
  return
      [NSRunningApplication
          runningApplicationsWithBundleIdentifier:base::SysUTF8ToNSString(
                                                      std::string(bundle_id))]
          .count > 0;
}

}  // namespace compass::browser_import
