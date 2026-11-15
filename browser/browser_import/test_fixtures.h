// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_TEST_FIXTURES_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_TEST_FIXTURES_H_

#include <cstdint>
#include <string>
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
// |encrypt|). |local_state_extra| is merged into Local State (os_crypt).
bool WriteChromiumUserData(const base::FilePath& user_data,
                           const std::string& profile_name,
                           const std::vector<FixtureTab>& tabs,
                           const Encrypt& encrypt,
                           const std::string& local_state_os_crypt_key = "");

}  // namespace compass::browser_import::test

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_TEST_FIXTURES_H_
