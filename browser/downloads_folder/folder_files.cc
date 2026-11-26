// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/downloads_folder/folder_files.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"

namespace compass::downloads_folder {

namespace {

constexpr std::string_view kImageExtensions[] = {
    "png", "jpg", "jpeg", "gif", "webp", "heic", "heif", "bmp", "tif", "tiff",
    "avif"};
constexpr std::string_view kVideoExtensions[] = {"mov", "mp4", "m4v", "webm",
                                                 "mkv", "avi"};
// Partial downloads: Chromium's, Safari's (a bundle) and Firefox's.
constexpr std::string_view kPartialExtensions[] = {"crdownload", "download",
                                                   "part"};
// Folder metadata, compared case-insensitively.
constexpr std::string_view kMetadataNames[] = {"desktop.ini", "thumbs.db"};

std::string LowerExtension(const base::FilePath& path) {
  std::string extension = base::FilePath(path.BaseName()).Extension();
  if (!extension.empty() && extension[0] == '.') {
    extension.erase(0, 1);
  }
  return base::ToLowerASCII(extension);
}

bool Contains(base::span<const std::string_view> list, std::string_view s) {
  return std::ranges::find(list, s) != list.end();
}

base::FilePath& ScreenshotFolderOverride() {
  static base::NoDestructor<base::FilePath> folder;
  return *folder;
}

bool NewerFirst(const FolderFile& a, const FolderFile& b) {
  if (a.added != b.added) {
    return a.added > b.added;
  }
  return a.path < b.path;
}

}  // namespace

base::FilePath ScreenshotFolder() {
  if (!ScreenshotFolderOverride().empty()) {
    return ScreenshotFolderOverride();
  }
  return SystemScreenshotFolder();
}

void SetScreenshotFolderForTesting(const base::FilePath& folder) {
  ScreenshotFolderOverride() = folder;
}

FileKind KindForPath(const base::FilePath& path) {
  const std::string extension = LowerExtension(path);
  if (Contains(kImageExtensions, extension)) {
    return FileKind::kImage;
  }
  if (Contains(kVideoExtensions, extension)) {
    return FileKind::kVideo;
  }
  return FileKind::kOther;
}

bool IsListable(const base::FilePath& name) {
  const std::string base_name = name.BaseName().AsUTF8Unsafe();
  if (base_name.empty() || base_name[0] == '.') {
    return false;  // Hidden, .DS_Store, .localized, .com.google.Chrome.XXXX.
  }
  if (Contains(kMetadataNames, base::ToLowerASCII(base_name))) {
    return false;
  }
  return !Contains(kPartialExtensions, LowerExtension(name));
}

std::vector<FolderFile> ListFolder(const base::FilePath& folder) {
  std::vector<FolderFile> files;
  base::FileEnumerator enumerator(
      folder, /*recursive=*/false,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    if (!IsListable(path)) {
      continue;
    }
    const base::FileEnumerator::FileInfo info = enumerator.GetInfo();
    FolderFile file;
    file.path = path;
    file.is_directory = info.IsDirectory();
    file.size = file.is_directory ? 0 : info.GetSize();
    file.kind = file.is_directory ? FileKind::kOther : KindForPath(path);
    std::optional<base::Time> added = AddedToFolderTime(path);
    file.added = added.value_or(info.GetLastModifiedTime());
    if constexpr (kSystemMarksScreenCaptures) {
      file.is_screen_capture =
          file.kind != FileKind::kOther && IsScreenCapture(path);
    }
    files.push_back(std::move(file));
  }
  std::ranges::sort(files, NewerFirst);
  return files;
}

std::vector<FolderFile> DownloadsTabFiles(
    std::vector<FolderFile> listing,
    const base::flat_set<base::FilePath>& skip,
    bool hide_screen_captures,
    size_t limit) {
  std::vector<FolderFile> files;
  for (FolderFile& file : listing) {
    if (files.size() >= limit) {
      break;
    }
    if (skip.contains(file.path)) {
      continue;
    }
    if (hide_screen_captures && file.is_screen_capture) {
      continue;
    }
    files.push_back(std::move(file));
  }
  return files;
}

std::vector<FolderFile> ScreenshotsTabFiles(std::vector<FolderFile> listing,
                                            size_t limit) {
  std::vector<FolderFile> files;
  for (FolderFile& file : listing) {
    if (files.size() >= limit) {
      break;
    }
    if (file.is_directory || file.kind == FileKind::kOther) {
      continue;
    }
    if (kSystemMarksScreenCaptures && !file.is_screen_capture) {
      continue;
    }
    files.push_back(std::move(file));
  }
  return files;
}

}  // namespace compass::downloads_folder
