// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_PROFILE_READER_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_PROFILE_READER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/values.h"
#include "compass/browser/browser_import/source.h"

// Reading what's chosen from one profile of another browser, whichever it
// is (docs/design/browser-import.md).
namespace compass::browser_import {

struct ReadOptions {
  // Bookmarks to Pinned.
  bool bookmarks = true;
  // Open tabs to Today.
  bool tabs = true;
  // History and passwords, where Compass reads them itself (Chromium-based
  // browsers, Safari's history). Firefox-based browsers' go through
  // Chromium's own importer instead (ImportsHistoryThroughChromium()).
  bool history = false;
  bool passwords = false;
};

// A Chromium-based profile's sidebar, read but not built yet: its open tabs
// need upstream's session restore, which draws session IDs, and only the UI
// thread may (SessionIdGenerator). FinishReadingProfile() builds it.
struct UnbuiltChromiumSidebar {
  UnbuiltChromiumSidebar();
  UnbuiltChromiumSidebar(UnbuiltChromiumSidebar&&);
  UnbuiltChromiumSidebar& operator=(UnbuiltChromiumSidebar&&);
  ~UnbuiltChromiumSidebar();

  SourceProfile profile;
  ReadOptions options;
  std::optional<base::DictValue> bookmarks;
  std::optional<base::DictValue> preferences;
  // Newest first.
  std::vector<std::vector<uint8_t>> session_files;
};

struct ProfileData {
  ProfileData();
  ProfileData(ProfileData&&);
  ProfileData& operator=(ProfileData&&);
  ~ProfileData();

  // Empty until FinishReadingProfile() for a Chromium-based profile.
  ImportedSidebar sidebar;
  std::optional<UnbuiltChromiumSidebar> unbuilt;
  std::vector<HistoryEntry> history;
  std::vector<LoginEntry> logins;
  // Saved passwords that couldn't be decrypted.
  int logins_skipped = 0;
  // Passwords were asked for but the browser's key couldn't be read.
  bool no_password_key = false;
  // What couldn't be read, in words for the page.
  std::vector<std::string> problems;
};

// Reads |profile| as |options| say. Blocking (and, for passwords on macOS,
// may ask the user for Keychain access).
ProfileData ReadProfile(const SourceProfile& profile,
                        const ReadOptions& options);

// Builds |data|'s sidebar where ReadProfile() couldn't (Chromium-based
// profiles' open tabs, UnbuiltChromiumSidebar). Not blocking; on the UI
// thread in the browser. Does nothing for other profiles.
void FinishReadingProfile(ProfileData& data);

// What Compass can import from |browser| on this platform.
bool CanImportHistory(Browser browser);
bool CanImportPasswords(Browser browser);
// Whether |browser| keeps its history outside its Chromium profile folder
// (Crest, Yab), so a copy of the folder (CanImportAsProfile()) leaves it
// behind: it's read with ReadProfile() and written into the new profile.
bool KeepsHistoryOutsideProfile(Browser browser);
// Whether |browser|'s history and passwords go through Chromium's importer
// (ExternalProcessImporterHost) rather than ReadProfile().
bool ImportsHistoryThroughChromium(Browser browser);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_PROFILE_READER_H_
