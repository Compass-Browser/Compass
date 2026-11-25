// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/browser_import/app_icon.h"

#include <windows.h>

#include <shlobj.h>

#include <vector>

#include "base/base_paths_win.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/win/scoped_gdi_object.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/win/icon_util.h"

namespace compass::browser_import {

namespace {

// Where a browser's executable is, under Program Files (machine-wide) or
// %LOCALAPPDATA% (per user); empty for those Compass doesn't know.
base::FilePath::StringViewType InstallFolder(Browser browser) {
  switch (browser) {
    case Browser::kChrome:
      return FILE_PATH_LITERAL("Google\\Chrome\\Application");
    case Browser::kEdge:
      return FILE_PATH_LITERAL("Microsoft\\Edge\\Application");
    case Browser::kBrave:
      return FILE_PATH_LITERAL("BraveSoftware\\Brave-Browser\\Application");
    case Browser::kVivaldi:
      return FILE_PATH_LITERAL("Vivaldi\\Application");
    case Browser::kFirefox:
      return FILE_PATH_LITERAL("Mozilla Firefox");
    case Browser::kZen:
      return FILE_PATH_LITERAL("Zen Browser");
    default:
      return FILE_PATH_LITERAL("");
  }
}

base::FilePath::StringViewType Executable(Browser browser) {
  switch (browser) {
    case Browser::kChrome:
      return FILE_PATH_LITERAL("chrome.exe");
    case Browser::kEdge:
      return FILE_PATH_LITERAL("msedge.exe");
    case Browser::kBrave:
      return FILE_PATH_LITERAL("brave.exe");
    case Browser::kVivaldi:
      return FILE_PATH_LITERAL("vivaldi.exe");
    case Browser::kFirefox:
      return FILE_PATH_LITERAL("firefox.exe");
    case Browser::kZen:
      return FILE_PATH_LITERAL("zen.exe");
    default:
      return FILE_PATH_LITERAL("");
  }
}

// The executable of |profile|'s browser, or empty if it isn't found.
base::FilePath FindExecutable(const SourceProfile& profile) {
  const base::FilePath::StringViewType exe = Executable(profile.browser);
  if (exe.empty()) {
    return base::FilePath();
  }
  std::vector<base::FilePath> candidates;
  // Firefox-based: the profile names its app's folder.
  if (!profile.app_path.empty()) {
    candidates.push_back(profile.app_path.Append(exe));
  }
  // Chromium-based, per-user installs: "Application" next to User Data.
  if (IsChromiumBased(profile.browser) && !profile.root.empty()) {
    candidates.push_back(
        profile.root.DirName().Append(FILE_PATH_LITERAL("Application"))
            .Append(exe));
  }
  for (const int key : {base::DIR_PROGRAM_FILES, base::DIR_PROGRAM_FILESX86,
                        base::DIR_LOCAL_APP_DATA}) {
    base::FilePath dir;
    if (base::PathService::Get(key, &dir)) {
      candidates.push_back(
          dir.Append(InstallFolder(profile.browser)).Append(exe));
    }
  }
  for (const base::FilePath& candidate : candidates) {
    if (base::PathExists(candidate)) {
      return candidate;
    }
  }
  return base::FilePath();
}

}  // namespace

std::string BrowserAppIcon(const SourceProfile& profile) {
  const base::FilePath exe = FindExecutable(profile);
  if (exe.empty()) {
    return std::string();
  }
  HICON icon = nullptr;
  if (SHDefExtractIcon(exe.value().c_str(), 0, 0, &icon, nullptr,
                       MAKELONG(kAppIconSize, 0)) != S_OK ||
      !icon) {
    return std::string();
  }
  base::win::ScopedGDIObject<HICON> scoped_icon(icon);
  return PngDataUrl(IconUtil::CreateSkBitmapFromHICON(scoped_icon.get()));
}

// Arc is an MSIX package: its icon isn't in an executable Compass can
// read. The page shows a letter.
std::string ArcAppIcon() {
  return std::string();
}

}  // namespace compass::browser_import
