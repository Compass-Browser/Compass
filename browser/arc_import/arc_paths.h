// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ARC_IMPORT_ARC_PATHS_H_
#define COMPASS_BROWSER_ARC_IMPORT_ARC_PATHS_H_

#include <optional>

#include "base/files/file_path.h"

namespace compass::arc {

// Where Arc keeps its data (docs/design/arc-import.md, "What Arc keeps
// where").
struct ArcPaths {
  // Its Chromium profiles ("User Data", with Local State).
  base::FilePath user_data;
  // Their disk caches, by profile folder; empty where Chromium keeps the
  // cache inside the profile (Windows), so it comes with the profile.
  base::FilePath cache;
  // StorableSidebar.json.
  base::FilePath sidebar;
  // StorableArchiveItems.json.
  base::FilePath archive;
  // Arc's Firestore cache, a LevelDB with its Easels (arc_easels.h); macOS
  // only, empty elsewhere.
  base::FilePath easels;
};

// Arc's folders and files under |arc|, the folder that holds its User Data
// and StorableSidebar.json; |cache|: the folder of its profiles' caches, or
// empty.
ArcPaths ArcPathsIn(const base::FilePath& arc, const base::FilePath& cache);

// macOS: ~/Library/Application Support/Arc, the caches in ~/Library/Caches/Arc.
// |app_support| and |caches| are those two Library folders.
ArcPaths MacArcPaths(const base::FilePath& app_support,
                     const base::FilePath& caches);

// The name of Arc for Windows' MSIX package; its folders are
// "<name>_<publisher ID>" (TheBrowserCompany.Arc_ttt1ap7aakyb4).
inline constexpr char kArcWindowsPackageName[] = "TheBrowserCompany.Arc";

// Windows: Arc is an MSIX package, so what it writes to %LOCALAPPDATA%\Arc
// lands in %LOCALAPPDATA%\Packages\TheBrowserCompany.Arc_<publisher ID>\
// LocalCache\Local\Arc. Finds the first package folder (by name) whose Arc
// folder has a User Data folder; nullopt if there's none. |local_app_data|:
// %LOCALAPPDATA%. Blocking.
std::optional<ArcPaths> FindWindowsArcPaths(
    const base::FilePath& local_app_data);

}  // namespace compass::arc

#endif  // COMPASS_BROWSER_ARC_IMPORT_ARC_PATHS_H_
