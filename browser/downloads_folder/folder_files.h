// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_DOWNLOADS_FOLDER_FOLDER_FILES_H_
#define COMPASS_BROWSER_DOWNLOADS_FOLDER_FOLDER_FILES_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "base/containers/flat_set.h"
#include "base/files/file_path.h"
#include "base/time/time.h"
#include "build/build_config.h"

// What the downloads bubble's tabs list (docs/design/downloads-bubble.md):
// the files in the download folder, whoever put them there, and the
// screenshots in the system's screenshot folder. Listing touches the disk,
// so it runs on a MayBlock sequence; the filters are pure.
namespace compass::downloads_folder {

enum class FileKind { kImage, kVideo, kOther };

struct FolderFile {
  base::FilePath path;
  int64_t size = 0;
  // When it arrived in the folder: macOS records it, Windows' creation time
  // is when the file was made there; else its last change.
  base::Time added;
  bool is_directory = false;
  // macOS marked it as a screenshot or a screen recording.
  bool is_screen_capture = false;
  FileKind kind = FileKind::kOther;
};

// By extension, case-insensitively.
FileKind KindForPath(const base::FilePath& path);

// Whether a folder entry is shown at all: not hidden files, folder metadata
// (.DS_Store, desktop.ini, .localized) or partial downloads (.crdownload,
// Safari's .download, Firefox's .part).
bool IsListable(const base::FilePath& name);

// Every listable entry directly in |folder|, newest first. Blocking.
std::vector<FolderFile> ListFolder(const base::FilePath& folder);

// The Downloads tab's files: |listing| without |skip| (the browser's own
// rows already show those) and, when |hide_screen_captures| (the screenshot
// folder is the download folder), without screenshots; at most |limit|.
std::vector<FolderFile> DownloadsTabFiles(
    std::vector<FolderFile> listing,
    const base::flat_set<base::FilePath>& skip,
    bool hide_screen_captures,
    size_t limit);

// The Screenshots tab's files: on macOS the ones the system marked (a
// folder like the Desktop holds other images too); elsewhere every image and
// video in the screenshot folder. At most |limit|.
std::vector<FolderFile> ScreenshotsTabFiles(std::vector<FolderFile> listing,
                                            size_t limit);

// Whether the system marks screenshots it saves (macOS's
// kMDItemIsScreenCapture), so the Screenshots tab can tell them apart.
inline constexpr bool kSystemMarksScreenCaptures = BUILDFLAG(IS_MAC);

// Where screenshots go: SystemScreenshotFolder(), unless a test set one.
// Blocking.
base::FilePath ScreenshotFolder();
void SetScreenshotFolderForTesting(const base::FilePath& folder);

// Platform parts (folder_files_{mac,win,linux}): blocking.

// Where the system saves screenshots: on macOS Screenshot.app's choice
// (com.apple.screencapture's location-screenshot, else location, else the
// Desktop); on Windows Pictures\Screenshots; on Linux the XDG pictures
// folder's Screenshots. Read each time: the user can change it any time.
base::FilePath SystemScreenshotFolder();

// Whether macOS marked |path| as a screen capture. False elsewhere.
bool IsScreenCapture(const base::FilePath& path);

// Marks |path| as a screen capture the way macOS does, so Compass's own
// screenshots list with the system's. Does nothing elsewhere.
void MarkAsScreenCapture(const base::FilePath& path);

// When |path| arrived in its folder, if the system records it: macOS's
// date added, Windows' creation time. None on Linux.
std::optional<base::Time> AddedToFolderTime(const base::FilePath& path);

// Moves |path| to the Trash (the Recycle Bin), so it can be put back.
bool MoveToTrash(const base::FilePath& path);

}  // namespace compass::downloads_folder

#endif  // COMPASS_BROWSER_DOWNLOADS_FOLDER_FOLDER_FILES_H_
