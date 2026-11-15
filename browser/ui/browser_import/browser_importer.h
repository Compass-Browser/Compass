// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_BROWSER_IMPORT_BROWSER_IMPORTER_H_
#define COMPASS_BROWSER_UI_BROWSER_IMPORT_BROWSER_IMPORTER_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/uuid.h"
#include "compass/browser/browser_import/source.h"

class Profile;

// Importing from other browsers into a Compass profile
// (docs/design/browser-import.md): what the import page shows, and the
// import itself.
namespace compass::browser_import {

// A profile of another browser, as the page lists it.
struct ProfileOverview {
  ProfileOverview();
  ProfileOverview(const ProfileOverview&);
  ProfileOverview& operator=(const ProfileOverview&);
  ~ProfileOverview();

  SourceProfile source;
  // The spaces it would bring, in order.
  std::vector<std::u16string> spaces;
  // Tabs its bookmarks give (Pinned), its open tabs (Today) and favorites.
  int bookmarks = 0;
  int tabs = 0;
  // Its browser is running: quit it first.
  bool running = false;
  // Its spaces were all imported into this Compass profile before.
  bool imported = false;
  // What can come along on this platform.
  bool history = false;
  bool passwords = false;
};

// The profiles of other browsers on this computer, for |target| (whose
// spaces say what was imported). On the UI thread.
void GetBrowserImportOverview(
    Profile* target,
    base::OnceCallback<void(std::vector<ProfileOverview>)> callback);

// What to import from one profile (ProfileOverview::source.id).
struct BrowserImportChoice {
  std::string id;
  bool bookmarks = true;
  bool tabs = true;
  bool history = true;
  bool passwords = true;
};

// What one profile's import did, for the page's summary.
struct BrowserImportSummary {
  BrowserImportSummary();
  BrowserImportSummary(const BrowserImportSummary&);
  BrowserImportSummary& operator=(const BrowserImportSummary&);
  ~BrowserImportSummary();

  // The profile, as its space is named ("Chrome – Work").
  std::u16string name;
  // The spaces it made.
  std::vector<std::u16string> spaces;
  int pinned = 0;
  int today = 0;
  int favorites = 0;
  // History pages and passwords handed to Compass's history and password
  // store (-1: through Chromium's importer, which doesn't count them).
  int history = 0;
  int passwords = 0;
  // Passwords that couldn't be decrypted.
  int passwords_skipped = 0;
  // Things that didn't work, in words for the page.
  std::vector<std::string> problems;
};

struct BrowserImportResult {
  BrowserImportResult();
  BrowserImportResult(BrowserImportResult&&);
  BrowserImportResult& operator=(BrowserImportResult&&);
  ~BrowserImportResult();

  std::vector<BrowserImportSummary> summaries;
  // The first space made, to show it.
  base::Uuid first_space;
};

// Imports |choices| into |target|, one profile after another: their spaces
// (AddImportedSidebar()), history and passwords. On the UI thread.
void ImportFromBrowsers(
    Profile* target,
    std::vector<BrowserImportChoice> choices,
    base::OnceCallback<void(BrowserImportResult)> callback);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_UI_BROWSER_IMPORT_BROWSER_IMPORTER_H_
