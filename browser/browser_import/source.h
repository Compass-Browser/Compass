// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_SOURCE_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_SOURCE_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "compass/browser/arc_import/arc_sidebar.h"
#include "url/gurl.h"

// Importing from other browsers (docs/design/browser-import.md): what the
// readers share. Plain data, no browser dependencies. The sidebar a profile
// brings is expressed with the Arc import's structures (arc::Space,
// arc::Item), which AddImportedSidebar() (space_import.h) adds.
namespace compass::browser_import {

// The browsers Compass imports from, besides Arc (docs/design/arc-import.md).
enum class Browser {
  kChrome,
  kEdge,
  kBrave,
  kVivaldi,
};

inline constexpr Browser kAllBrowsers[] = {
    Browser::kChrome,
    Browser::kEdge,
    Browser::kBrave,
    Browser::kVivaldi,
};

// As the page and the spaces name it: "Chrome", "Edge", ...
std::u16string BrowserName(Browser browser);
// For IDs: "chrome", "edge", ...
std::string_view BrowserKey(Browser browser);
std::optional<Browser> BrowserFromKey(std::string_view key);

// Chromium-based: profiles in a "User Data" folder (chromium_reader.h).
bool IsChromiumBased(Browser browser);

// One profile of another browser on this computer.
struct SourceProfile {
  SourceProfile();
  SourceProfile(const SourceProfile&);
  SourceProfile& operator=(const SourceProfile&);
  ~SourceProfile();

  Browser browser = Browser::kChrome;
  // The profile's folder: "<User Data>/Default", a Firefox profile folder,
  // ~/Library/Safari.
  base::FilePath path;
  // Where the browser keeps its profiles: Chromium's User Data (Local
  // State), the folder of Firefox's profiles.ini.
  base::FilePath root;
  // Firefox-based: the app's folder, where its NSS libraries are (read from
  // the profile's compatibility.ini). May be empty.
  base::FilePath app_path;
  // As the browser names it; empty for Safari.
  std::u16string name;
  // Whether the browser has other profiles (the space names say which).
  bool several = false;
  // Stable, for the page and for IDs: "<browser key>:<folder name>".
  std::string id;
};

// "<Browser>", or "<Browser> – <profile name>" for one of several.
std::u16string SpaceNameFor(const SourceProfile& profile);

// What a profile brings to the sidebar: spaces (one for its bookmarks and
// open tabs, or one per workspace), and favorites (Zen's essentials).
// Item and space IDs are namespaced by the profile's ID, so they derive the
// same Compass IDs on each import (arc::ArcDerivedId()).
struct ImportedSidebar {
  ImportedSidebar();
  ImportedSidebar(const ImportedSidebar&);
  ImportedSidebar(ImportedSidebar&&);
  ImportedSidebar& operator=(const ImportedSidebar&);
  ImportedSidebar& operator=(ImportedSidebar&&);
  ~ImportedSidebar();

  std::vector<arc::Space> spaces;
  std::vector<arc::Item> favorites;
};

// A page of the browser's history.
struct HistoryEntry {
  GURL url;
  std::u16string title;
  int visit_count = 0;
  int typed_count = 0;
  base::Time last_visit;
};

// A saved password, decrypted.
struct LoginEntry {
  LoginEntry();
  LoginEntry(const LoginEntry&);
  LoginEntry& operator=(const LoginEntry&);
  ~LoginEntry();

  GURL url;
  GURL action;
  std::string signon_realm;
  std::u16string username_element;
  std::u16string username;
  std::u16string password_element;
  std::u16string password;
  base::Time created;
  // Chromium's PasswordForm::Scheme (0 is HTML).
  int scheme = 0;
  // "Never save" for the site: no username or password.
  bool blocked = false;
};

// History pages imported at most, the most recently visited.
inline constexpr size_t kMaxHistoryEntries = 50000;

// Tabs in |items| (in folders and splits too).
int CountTabs(const std::vector<arc::Item>& items);

// A tab or folder of the sidebar to be.
arc::Item MakeTab(std::string id,
                  const GURL& url,
                  std::u16string title,
                  base::Time last_active = base::Time());
arc::Item MakeFolder(std::string id,
                     std::u16string title,
                     std::vector<arc::Item> children);

// Copies a database at |from| (with its -wal and -journal files) into
// |dir| so it can be read while its browser has it, without touching the
// original. Returns the copy, or an empty path. Blocking.
base::FilePath CopyDatabase(const base::FilePath& from,
                            const base::FilePath& dir);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_SOURCE_H_
