// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_FIREFOX_READER_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_FIREFOX_READER_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/values.h"
#include "compass/browser/browser_import/chromium_reader.h"
#include "compass/browser/browser_import/source.h"

// Reading Firefox-based browsers' profiles (Firefox, Zen): profiles.ini,
// places.sqlite's bookmarks, the session store (mozLz4-compressed JSON),
// and Zen's spaces, pinned tabs, folders and essentials. See
// docs/design/browser-import.md, "Firefox and Zen".
namespace compass::browser_import {

// A profile profiles.ini lists.
struct FirefoxProfileInfo {
  std::u16string name;
  base::FilePath path;
  // The one the browser opens (its install's Default, else Default=1).
  bool is_default = false;
};

// The profiles of |ini| (profiles.ini's text), the default first, then in
// the file's order. Relative paths are taken from |root|.
std::vector<FirefoxProfileInfo> ParseProfilesIni(std::string_view ini,
                                                 const base::FilePath& root);

// Firefox's mozLz4 files: "mozLz40\0", the decompressed size (32 bits,
// little-endian), then an LZ4 block. nullopt if |file| isn't one.
std::optional<std::string> DecompressMozLz4(base::span<const uint8_t> file);
// For tests: a mozLz4 file of |text| (literals only, which LZ4 allows).
std::vector<uint8_t> CompressMozLz4ForTesting(std::string_view text);
// A mozLz4 file's JSON object. Blocking.
std::optional<base::DictValue> ReadMozLz4Json(const base::FilePath& path);

// The Pinned entries places.sqlite's bookmarks give: the bookmarks
// toolbar's entries, then "Bookmarks Menu", "Other Bookmarks" and "Mobile
// Bookmarks" as folders (when not empty). Queries (place:) and separators
// are left out. IDs: |id_prefix| + the bookmark's GUID. Reads a copy.
// Blocking.
std::vector<arc::Item> ReadFirefoxBookmarks(const base::FilePath& places,
                                            std::string_view id_prefix);

// The open tabs of a session store (sessionstore.jsonlz4's object), window
// after window. Tab groups keep their names.
std::vector<SessionTab> ParseFirefoxSessionTabs(const base::DictValue& session);

// The newest session store of |profile_dir| (sessionstore.jsonlz4, or
// sessionstore-backups/recovery.jsonlz4 while the browser runs or after a
// crash). Blocking.
std::optional<base::DictValue> ReadFirefoxSession(
    const base::FilePath& profile_dir);

// Zen's sidebar state: zen-sessions.jsonlz4 when it has spaces, else the
// session store's first window (which carries "spaces", "folders" and the
// tabs' Zen fields too). Blocking.
std::optional<base::DictValue> ReadZenSidebarState(
    const base::FilePath& profile_dir);

// A Firefox profile's sidebar: one space named after the profile, with the
// bookmarks in Pinned and the open tabs in Today.
ImportedSidebar FirefoxSidebar(const SourceProfile& profile,
                               std::vector<arc::Item> bookmarks,
                               const base::DictValue* session,
                               bool import_tabs);

// A Zen profile's sidebar (docs/design/browser-import.md, "Zen"): a space
// per Zen space (name, icon, theme color), its pinned tabs and folders in
// Pinned (split views in folders as splits), its other tabs in Today (tab
// groups as folders), essentials as Favorites; the bookmarks, when there
// are any, in one more space named after the profile. |state| is
// ReadZenSidebarState()'s.
ImportedSidebar ZenSidebar(const SourceProfile& profile,
                           std::vector<arc::Item> bookmarks,
                           const base::DictValue* state,
                           bool import_tabs);

// The app's folder a profile was last used with (compatibility.ini's
// LastPlatformDir), where its NSS libraries are. Blocking.
base::FilePath FirefoxAppPath(const base::FilePath& profile_dir);

// Whether a Firefox-based browser has |profile_dir| open (its profile
// lock is held). Blocking.
bool IsFirefoxProfileInUse(const base::FilePath& profile_dir);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_FIREFOX_READER_H_
