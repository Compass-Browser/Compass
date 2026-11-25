// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/browser_import/app_icon.h"

#import <AppKit/AppKit.h>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "skia/ext/skia_utils_mac.h"
#include "third_party/skia/include/core/SkBitmap.h"

namespace compass::browser_import {

namespace {

// The browsers' bundle IDs; nullptr where Compass doesn't know it (the app
// is then looked for by name in /Applications).
NSString* BundleId(Browser browser) {
  switch (browser) {
    case Browser::kChrome:
      return @"com.google.Chrome";
    case Browser::kEdge:
      return @"com.microsoft.edgemac";
    case Browser::kBrave:
      return @"com.brave.Browser";
    case Browser::kVivaldi:
      return @"com.vivaldi.Vivaldi";
    case Browser::kDia:
      return @"company.thebrowser.dia";
    case Browser::kCrest:
      return @"com.pauldavis.crest";
    case Browser::kYab:
      return @"dev.serafim.fast";
    case Browser::kFirefox:
      return @"org.mozilla.firefox";
    case Browser::kZen:
      return @"app.zen-browser.zen";
    case Browser::kSafari:
      return @"com.apple.Safari";
    case Browser::kArdia:
    case Browser::kCra:
      return nil;
  }
}

// The .app folder |path| is in (a Firefox-based profile's app_path is its
// Contents/MacOS or Contents/Resources), or empty.
base::FilePath EnclosingApp(base::FilePath path) {
  while (!path.empty() && path != path.DirName()) {
    if (path.Extension() == ".app") {
      return path;
    }
    path = path.DirName();
  }
  return base::FilePath();
}

std::string IconOf(NSURL* app) {
  if (!app) {
    return std::string();
  }
  NSImage* image = [[NSWorkspace sharedWorkspace] iconForFile:app.path];
  if (!image) {
    return std::string();
  }
  image = [image copy];
  image.size = NSMakeSize(kAppIconSize, kAppIconSize);
  return PngDataUrl(skia::NSImageToSkBitmap(image, /*is_opaque=*/false));
}

NSURL* AppWithBundleId(NSString* bundle_id) {
  return bundle_id ? [[NSWorkspace sharedWorkspace]
                         URLForApplicationWithBundleIdentifier:bundle_id]
                   : nil;
}

}  // namespace

std::string BrowserAppIcon(const SourceProfile& profile) {
  NSURL* app = AppWithBundleId(BundleId(profile.browser));
  if (!app) {
    const base::FilePath from_profile = EnclosingApp(profile.app_path);
    const base::FilePath by_name =
        base::FilePath("/Applications")
            .Append(base::UTF16ToUTF8(BrowserName(profile.browser)) + ".app");
    const base::FilePath path = !from_profile.empty() ? from_profile
                                : base::DirectoryExists(by_name)
                                    ? by_name
                                    : base::FilePath();
    if (!path.empty()) {
      app = [NSURL fileURLWithPath:base::SysUTF8ToNSString(path.value())];
    }
  }
  return IconOf(app);
}

std::string ArcAppIcon() {
  return IconOf(AppWithBundleId(@"company.thebrowser.Browser"));
}

}  // namespace compass::browser_import
