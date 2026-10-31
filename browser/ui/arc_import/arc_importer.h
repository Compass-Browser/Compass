// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_ARC_IMPORT_ARC_IMPORTER_H_
#define COMPASS_BROWSER_UI_ARC_IMPORT_ARC_IMPORTER_H_

#include <optional>
#include <string>
#include <set>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/uuid.h"
#include "compass/browser/arc_import/arc_sidebar.h"
#include "compass/browser/arc_import/profile_copy.h"

class Profile;

namespace compass::arc {

// Where Arc keeps its data (docs/design/arc-import.md).
struct ArcPaths {
  // Its Chromium profiles: ~/Library/Application Support/Arc/User Data.
  base::FilePath user_data;
  // Their disk caches: ~/Library/Caches/Arc/User Data.
  base::FilePath cache;
  // ~/Library/Application Support/Arc/StorableSidebar.json.
  base::FilePath sidebar;
};

ArcPaths GetArcPaths();
void SetArcPathsForTesting(std::optional<ArcPaths> paths);

// Arc's Keychain password ("Arc Safe Storage"), or nullopt if there's none
// or the user didn't allow Compass to read it. macOS asks the first time.
// Blocking.
std::optional<std::string> ReadArcKeychainPassword();
void SetArcKeychainPasswordForTesting(std::optional<std::string> password);

struct ProfileImport {
  // The loaded Compass profile, or null if the import failed.
  raw_ptr<::Profile> profile = nullptr;
  CopyResult copy;
};

// Imports the Arc profile |arc_dir| ("Default", "Profile 1", ...) as a new
// Compass profile named |name|: copies it with its cache, re-encrypts its
// cookies and passwords with Compass's key, signs its preferences for
// Compass (Arc's signatures would make Compass reset them, extensions
// included), then loads it. Arc must not be running. On the UI thread.
void ImportArcProfile(const std::string& arc_dir,
                      const std::u16string& name,
                      base::OnceCallback<void(ProfileImport)> callback);

struct ArcImportResult {
  ArcImportResult();
  ArcImportResult(ArcImportResult&&);
  ArcImportResult& operator=(ArcImportResult&&);
  ~ArcImportResult();

  // False if Arc's sidebar couldn't be read; profiles may still have failed.
  bool ok = false;
  // The Compass profiles made, in Arc's order.
  std::vector<raw_ptr<::Profile>> profiles;
  // Arc profile folders that failed (copy, Keychain, load).
  std::vector<std::string> failed;
  // The first space imported, to show it.
  base::Uuid first_space;
};

// Imports everything from Arc: each of its profiles (ImportArcProfile()),
// then that profile's spaces, Pinned and Today entries and favorites
// (AddArcSidebar()), and lists the profile so its spaces show in the
// switcher. Profiles one after another. Arc must not be running. On the UI
// thread. Only the Arc profiles in |dirs|, or all if it's empty.
void ImportFromArc(std::vector<std::string> dirs,
                   base::OnceCallback<void(ArcImportResult)> callback);

// What Arc has, for the import page.
struct ArcOverview {
  ArcOverview();
  ArcOverview(ArcOverview&&);
  ArcOverview& operator=(ArcOverview&&);
  ~ArcOverview();

  std::optional<Sidebar> sidebar;
  // Arc profile folders a Compass profile was imported from already.
  std::set<std::string> imported;
};
void GetArcOverview(base::OnceCallback<void(ArcOverview)> callback);

// Whether Arc is open, and asking it to quit (macOS). QuitArc() returns
// whether it asked.
bool IsArcRunning();
bool QuitArc();
void SetArcRunningForTesting(std::optional<bool> running);

// Merges an Arc profile's Preferences and Secure Preferences into one
// dictionary without Arc's signatures, records where it came from
// (`compass.arc_import.profile_dir`), and writes it signed for Compass, as
// first run does with initial preferences. Removes Secure Preferences.
// Blocking.
bool SignImportedPreferences(const base::FilePath& profile_dir,
                             const std::string& arc_dir);

}  // namespace compass::arc

#endif  // COMPASS_BROWSER_UI_ARC_IMPORT_ARC_IMPORTER_H_
