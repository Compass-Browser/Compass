// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_PROFILE_READER_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_PROFILE_READER_H_

#include <string>
#include <vector>

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

struct ProfileData {
  ProfileData();
  ProfileData(ProfileData&&);
  ProfileData& operator=(ProfileData&&);
  ~ProfileData();

  ImportedSidebar sidebar;
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

// What Compass can import from |browser| on this platform.
bool CanImportHistory(Browser browser);
bool CanImportPasswords(Browser browser);
// Whether |browser|'s history and passwords go through Chromium's importer
// (ExternalProcessImporterHost) rather than ReadProfile().
bool ImportsHistoryThroughChromium(Browser browser);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_PROFILE_READER_H_
