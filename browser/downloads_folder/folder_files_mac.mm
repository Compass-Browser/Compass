// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/downloads_folder/folder_files.h"

#import <Foundation/Foundation.h>
#include <sys/xattr.h>

#include "base/apple/bridging.h"
#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/strings/sys_string_conversions.h"

namespace compass::downloads_folder {

namespace {

// Where macOS records that a file is a screenshot or a screen recording
// (Spotlight's kMDItemIsScreenCapture, as an extended attribute holding a
// binary property list's true).
constexpr char kScreenCaptureAttribute[] =
    "com.apple.metadata:kMDItemIsScreenCapture";

// A folder from com.apple.screencapture's |key|, if it names one that
// exists ("~/Downloads" is stored with its tilde).
std::optional<base::FilePath> ScreenCapturePref(CFStringRef key) {
  base::apple::ScopedCFTypeRef<CFPropertyListRef> value(
      CFPreferencesCopyAppValue(key, CFSTR("com.apple.screencapture")));
  CFStringRef string = base::apple::CFCast<CFStringRef>(value.get());
  if (!string) {
    return std::nullopt;
  }
  NSString* expanded =
      [base::apple::CFToNSPtrCast(string) stringByExpandingTildeInPath];
  if (expanded.length == 0) {
    return std::nullopt;
  }
  base::FilePath folder = base::apple::NSStringToFilePath(expanded);
  if (!base::DirectoryExists(folder)) {
    return std::nullopt;
  }
  return folder;
}

}  // namespace

base::FilePath SystemScreenshotFolder() {
  // Screenshot.app's Options write the preferences from another process;
  // read them fresh.
  CFPreferencesAppSynchronize(CFSTR("com.apple.screencapture"));
  // macOS 26 keeps screenshots' and recordings' folders apart; earlier
  // versions have one.
  for (CFStringRef key : {CFSTR("location-screenshot"), CFSTR("location")}) {
    if (std::optional<base::FilePath> folder = ScreenCapturePref(key)) {
      return *folder;
    }
  }
  base::FilePath desktop;
  base::PathService::Get(base::DIR_USER_DESKTOP, &desktop);
  return desktop;
}

bool IsScreenCapture(const base::FilePath& path) {
  // Only its presence matters: macOS writes it on captures only.
  return getxattr(path.value().c_str(), kScreenCaptureAttribute, nullptr, 0, 0,
                  0) > 0;
}

void MarkAsScreenCapture(const base::FilePath& path) {
  NSData* data = [NSPropertyListSerialization
      dataWithPropertyList:@YES
                    format:NSPropertyListBinaryFormat_v1_0
                   options:0
                     error:nil];
  if (!data) {
    return;
  }
  setxattr(path.value().c_str(), kScreenCaptureAttribute, data.bytes,
           data.length, 0, 0);
}

std::optional<base::Time> AddedToFolderTime(const base::FilePath& path) {
  NSURL* url = base::apple::FilePathToNSURL(path);
  NSDate* added = nil;
  if (![url getResourceValue:&added
                      forKey:NSURLAddedToDirectoryDateKey
                       error:nil] ||
      !added) {
    return std::nullopt;
  }
  return base::Time::FromNSDate(added);
}

bool MoveToTrash(const base::FilePath& path) {
  return [NSFileManager.defaultManager
          trashItemAtURL:base::apple::FilePathToNSURL(path)
        resultingItemURL:nil
                   error:nil];
}

}  // namespace compass::downloads_folder
