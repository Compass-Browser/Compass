// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_CHROMIUM_READER_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_CHROMIUM_READER_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/time/time.h"
#include "base/values.h"
#include "compass/browser/browser_import/source.h"
#include "url/gurl.h"

// Reading Chromium-based browsers' profiles (Chrome, Edge, Brave, Vivaldi):
// Local State's profile list, Bookmarks, the session files, History and
// Login Data. See docs/design/browser-import.md, "Chromium-based browsers".
namespace compass::browser_import {

// A profile Local State lists.
struct ChromiumProfileInfo {
  // Its folder in User Data ("Default", "Profile 1").
  std::string dir;
  std::u16string name;
};

// The profiles of |local_state|'s profile.info_cache, in the browser's
// order (profile.profiles_order), else "Default" first, then by folder.
std::vector<ChromiumProfileInfo> ParseChromiumProfiles(
    const base::DictValue& local_state);

// The Pinned entries a Bookmarks file gives: the bookmarks bar's entries,
// then "Other bookmarks" and "Mobile bookmarks" as folders (when not
// empty). Folders keep their nesting. IDs: |id_prefix| + the bookmark's
// GUID.
std::vector<arc::Item> ParseChromiumBookmarks(const base::DictValue& bookmarks,
                                              std::string_view id_prefix);

// An open tab of the last session.
struct SessionTab {
  SessionTab();
  SessionTab(const SessionTab&);
  SessionTab& operator=(const SessionTab&);
  ~SessionTab();

  GURL url;
  std::u16string title;
  bool pinned = false;
  // Its tab group, if any: the group's ID and title (may be empty).
  std::string group;
  std::u16string group_title;
  base::Time last_active;
  // Vivaldi: the workspace it's in (vivaldi.workspaces.list's id).
  std::string workspace;
};

// The tabs of a session file's normal windows, window after window, in tab
// strip order. |file| holds a whole session file (header and commands).
// Commands this Chromium doesn't know (newer ones, Vivaldi's) are left out;
// Vivaldi's tab data (vivExtData) gives the tabs' workspaces. nullopt if it
// isn't a session file Compass reads (encrypted ones aren't).
std::optional<std::vector<SessionTab>> ParseChromiumSessionFile(
    base::span<const uint8_t> file);

// The newest readable session file of |profile_dir| (its Sessions folder,
// "Session_<time>"), parsed. Empty if there's none. Blocking.
std::vector<SessionTab> ReadChromiumSession(const base::FilePath& profile_dir);

// Today's entries for |tabs|: each tab, and tab groups with a title as
// folders (tabs of untitled groups stay loose). Pages Compass doesn't
// restore (new tab pages, about: pages) are left out. IDs: |id_prefix| +
// the tab's place.
std::vector<arc::Item> TodayFromTabs(const std::vector<SessionTab>& tabs,
                                     std::string_view id_prefix);

// A Vivaldi workspace (Preferences: vivaldi.workspaces.list).
struct VivaldiWorkspace {
  std::string id;
  std::u16string name;
  // Its emoji icon, if it has one.
  std::string emoji;
};
std::vector<VivaldiWorkspace> ParseVivaldiWorkspaces(
    const base::DictValue& preferences);

// A Chromium profile's sidebar (docs/design/browser-import.md, "Mapping"):
// one space named |space_name| with the bookmarks in Pinned and the open
// tabs in Today; for Vivaldi, a space per workspace for the tabs in it.
// |bookmarks| (Bookmarks file), |preferences| and |tabs| may be null/empty.
ImportedSidebar ChromiumSidebar(const SourceProfile& profile,
                                const base::DictValue* bookmarks,
                                const base::DictValue* preferences,
                                const std::vector<SessionTab>& tabs,
                                bool import_bookmarks,
                                bool import_tabs);

// History's pages, the most recently visited first, at most |max|. Reads a
// copy of the database. Blocking.
std::vector<HistoryEntry> ReadChromiumHistory(const base::FilePath& history,
                                              size_t max);

// Decrypts a Login Data password_value; nullopt if it can't.
using PasswordDecryptor =
    base::RepeatingCallback<std::optional<std::string>(std::string_view)>;

// Login Data's saved passwords, decrypted with |decrypt|. Those that don't
// decrypt are counted in |undecryptable|. Reads a copy. Blocking.
std::vector<LoginEntry> ReadChromiumLogins(const base::FilePath& login_data,
                                           const PasswordDecryptor& decrypt,
                                           int* undecryptable);

// Whether the browser of |user_data| (Chromium's User Data folder) is
// running: its singleton lock is held. Blocking.
bool IsChromiumRunning(const base::FilePath& user_data);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_CHROMIUM_READER_H_
