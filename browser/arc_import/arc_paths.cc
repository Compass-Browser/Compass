// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/arc_paths.h"

#include <algorithm>
#include <string>
#include <vector>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"

namespace compass::arc {

ArcPaths ArcPathsIn(const base::FilePath& arc, const base::FilePath& cache) {
  ArcPaths paths;
  paths.user_data = arc.AppendASCII("User Data");
  paths.sidebar = arc.AppendASCII("StorableSidebar.json");
  paths.archive = arc.AppendASCII("StorableArchiveItems.json");
  paths.cache = cache;
  return paths;
}

ArcPaths MacArcPaths(const base::FilePath& app_support,
                     const base::FilePath& caches) {
  ArcPaths paths = ArcPathsIn(
      app_support.AppendASCII("Arc"),
      caches.empty() ? base::FilePath()
                     : caches.AppendASCII("Arc").AppendASCII("User Data"));
  // Firestore's iOS/macOS SDK keeps its cache by app, project and database.
  paths.easels = app_support.AppendASCII("firestore")
                     .AppendASCII("Arc")
                     .AppendASCII("bcny-arc-server")
                     .AppendASCII("main");
  return paths;
}

std::optional<ArcPaths> FindWindowsArcPaths(
    const base::FilePath& local_app_data) {
  if (local_app_data.empty()) {
    return std::nullopt;
  }
  const std::string prefix = std::string(kArcWindowsPackageName) + "_";
  std::vector<base::FilePath> packages;
  base::FileEnumerator entries(local_app_data.AppendASCII("Packages"),
                               /*recursive=*/false,
                               base::FileEnumerator::DIRECTORIES);
  for (base::FilePath entry = entries.Next(); !entry.empty();
       entry = entries.Next()) {
    if (entry.BaseName().AsUTF8Unsafe().starts_with(prefix)) {
      packages.push_back(entry);
    }
  }
  std::ranges::sort(packages);
  for (const base::FilePath& package : packages) {
    const base::FilePath arc = package.AppendASCII("LocalCache")
                                   .AppendASCII("Local")
                                   .AppendASCII("Arc");
    ArcPaths paths = ArcPathsIn(arc, base::FilePath());
    if (base::DirectoryExists(paths.user_data)) {
      return paths;
    }
  }
  return std::nullopt;
}

}  // namespace compass::arc
