// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_TEST_FIXTURES_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_TEST_FIXTURES_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"

// Synthetic profiles of other browsers, written the way those browsers
// write them, for the importer's tests. Nothing here comes from a real
// profile: the sites are example.test-style placeholders.
namespace compass::browser_import::test {

// A tab of a fixture session.
struct FixtureTab {
  std::string url;
  std::u16string title;
  bool pinned = false;
  // A tab group's title; tabs with the same one share a group.
  std::u16string group;
  // Vivaldi: the workspace ID its tab data names.
  std::string vivaldi_workspace;
  // In a tab group of its own with no title, as Dia keeps its pins.
  bool untitled_group = false;
};

// A Chromium session file (version 3) with one normal window per element
// of |windows|.
std::vector<uint8_t> MakeChromiumSessionFile(
    const std::vector<std::vector<FixtureTab>>& windows);

// A Bookmarks file: the bar has "Bar link" (https://bar.test/) and a
// folder "Docs" with "Guide" (https://docs.test/guide); Other bookmarks
// has "Other link" (https://other.test/).
std::string MakeChromiumBookmarksJson();

// Encrypts a password the way the browser would ("v10..."), or leaves it
// as is.
using Encrypt = base::RepeatingCallback<std::string(const std::string&)>;

// Writes a Chromium User Data folder at |user_data| with a profile
// "Default" named |profile_name|: Local State, Bookmarks
// (MakeChromiumBookmarksJson()), a session with |tabs| in one window,
// History (https://history.test/a and /b) and Login Data (one login for
// https://login.test/, user "user", password "secret", encrypted with
// |encrypt|). |local_state_os_crypt_key|, if any, is Local State's
// os_crypt.encrypted_key.
bool WriteChromiumUserData(const base::FilePath& user_data,
                           const std::string& profile_name,
                           const std::vector<FixtureTab>& tabs,
                           const Encrypt& encrypt,
                           const std::string& local_state_os_crypt_key = "");

// Crest's ControlPlane/session.sqlite at |path| (its folders made): the
// "core" row |core|, and a "history.<space ID>" row for each of |history|
// (space ID, JSON), as Crest writes them.
bool WriteCrestSession(
    const base::FilePath& path,
    std::string_view core,
    const std::vector<std::pair<std::string, std::string>>& history = {});

// A places.sqlite at |path|: the toolbar has "Toolbar link"
// (https://toolbar.test/) and a folder "News" with "Daily"
// (https://news.test/daily); Other Bookmarks has "Unfiled"
// (https://unfiled.test/); the menu has only a query (place:), left out.
bool WriteFirefoxPlaces(const base::FilePath& path);

// A Firefox session store (sessionstore.jsonlz4's JSON): one window with a
// tab, a tab group "Reading" of two tabs, and a pinned tab.
std::string MakeFirefoxSessionJson();

// Zen's zen-sessions.jsonlz4 JSON: spaces "Work" (an emoji, a blue theme)
// and "Play" (no theme); Work has a pinned folder "Docs" with a nested
// folder "Specs", a pinned tab whose page moved on, and an open tab; Play
// has an open tab; an essential; an empty tab and a Glance tab.
std::string MakeZenSessionsJson();

}  // namespace compass::browser_import::test

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_TEST_FIXTURES_H_
