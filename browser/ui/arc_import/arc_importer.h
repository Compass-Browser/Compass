// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_ARC_IMPORT_ARC_IMPORTER_H_
#define COMPASS_BROWSER_UI_ARC_IMPORT_ARC_IMPORTER_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/uuid.h"
#include "compass/browser/arc_import/arc_sidebar.h"
#include "compass/browser/arc_import/profile_copy.h"

class BrowserWindowInterface;
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
  // ~/Library/Application Support/Arc/StorableArchiveItems.json.
  base::FilePath archive;
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
  // Compass profiles imported before that got Arc's newer spaces.
  std::vector<raw_ptr<::Profile>> updated;
  // Arc profile folders that failed (copy, Keychain, load).
  std::vector<std::string> failed;
  // The first space imported, to show it.
  base::Uuid first_space;
};

// What to import (docs/design/arc-import.md, "Choosing", "Importing again").
// Nothing imported before comes in again.
struct ArcImportChoice {
  ArcImportChoice();
  ArcImportChoice(const ArcImportChoice&);
  ArcImportChoice& operator=(const ArcImportChoice&);
  ~ArcImportChoice();

  // The Arc profiles to import as new Compass profiles (folders: "Default",
  // "Profile 1", ...); nullopt: every one not imported yet.
  std::optional<std::vector<std::string>> profiles;
  // The Arc spaces (IDs) to bring in, into the new profiles or into the
  // Compass profiles their Arc profile was imported as before; nullopt:
  // every one not imported yet. Spaces of an Arc profile that is neither
  // imported nor chosen stay out.
  std::optional<std::set<std::string>> spaces;
};

// Imports from Arc: each chosen profile (ImportArcProfile()), then its
// spaces, Pinned and Today entries and favorites (AddArcSidebar()), and
// Arc's newer spaces of profiles imported before. Lists each profile so its
// spaces show in the switcher, and records the spaces it brought in
// (prefs::kArcImportSpaces). Profiles one after another. Arc must not be
// running. On the UI thread.
void ImportFromArc(ArcImportChoice choice,
                   base::OnceCallback<void(ArcImportResult)> callback);

// A Compass profile imported from Arc.
struct ImportedProfile {
  ImportedProfile();
  ImportedProfile(const ImportedProfile&);
  ImportedProfile& operator=(const ImportedProfile&);
  ~ImportedProfile();

  base::FilePath path;
  // The Arc spaces brought into it.
  std::set<std::string> spaces;
};

// What Arc has, for the import page.
struct ArcOverview {
  ArcOverview();
  ArcOverview(ArcOverview&&);
  ArcOverview& operator=(ArcOverview&&);
  ~ArcOverview();

  std::optional<Sidebar> sidebar;
  // By Arc profile folder: the Compass profile it was imported as.
  std::map<std::string, ImportedProfile> imported;
};
void GetArcOverview(base::OnceCallback<void(ArcOverview)> callback);

// On the first run, if Arc is on this Mac and nothing was imported from it,
// opens the import page (compass://settings/importArc) in the process's
// first normal window. Called by the sidebar of each window.
void MaybeOfferImportOnFirstRun(BrowserWindowInterface* browser);
// Tests: the next normal window is considered again, as a first run or not
// (nullopt: as first_run::IsChromeFirstRun() says).
void SetFirstRunForArcImportOfferForTesting(std::optional<bool> first_run);

// Whether Arc is open, and asking it to quit (macOS). QuitArc() returns
// whether it asked.
bool IsArcRunning();
bool QuitArc();
void SetArcRunningForTesting(std::optional<bool> running);

// Merges an Arc profile's Preferences and Secure Preferences into one
// dictionary without Arc's signatures, records where it came from
// (prefs::kArcImportProfileDir), and writes it signed for Compass, as
// first run does with initial preferences. Removes Secure Preferences.
// Blocking.
bool SignImportedPreferences(const base::FilePath& profile_dir,
                             const std::string& arc_dir);

}  // namespace compass::arc

#endif  // COMPASS_BROWSER_UI_ARC_IMPORT_ARC_IMPORTER_H_
