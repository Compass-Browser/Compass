// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/profile_reader.h"

#include <optional>
#include <utility>

#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "build/build_config.h"
#include "compass/browser/browser_import/chromium_crypto.h"
#include "compass/browser/browser_import/chromium_reader.h"
#include "compass/browser/browser_import/firefox_reader.h"
#include "compass/browser/browser_import/safari_reader.h"

namespace compass::browser_import {

namespace {

std::optional<base::DictValue> ReadJsonDict(const base::FilePath& path) {
  std::string json;
  if (!base::ReadFileToString(path, &json)) {
    return std::nullopt;
  }
  std::optional<base::Value> value = base::JSONReader::Read(
      json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!value || !value->is_dict()) {
    return std::nullopt;
  }
  return std::move(*value).TakeDict();
}

void ReadChromiumProfile(const SourceProfile& profile,
                         const ReadOptions& options,
                         ProfileData& data) {
  std::optional<base::DictValue> bookmarks;
  if (options.bookmarks) {
    bookmarks = ReadJsonDict(profile.path.AppendASCII("Bookmarks"));
  }
  std::optional<base::DictValue> preferences;
  std::vector<SessionTab> tabs;
  if (options.tabs) {
    tabs = ReadChromiumSession(profile.path);
    if (profile.browser == Browser::kVivaldi) {
      preferences = ReadJsonDict(profile.path.AppendASCII("Preferences"));
    }
  }
  data.sidebar = ChromiumSidebar(profile, bookmarks ? &*bookmarks : nullptr,
                                 preferences ? &*preferences : nullptr, tabs,
                                 options.bookmarks, options.tabs);
  if (options.history) {
    data.history = ReadChromiumHistory(profile.path.AppendASCII("History"),
                                       kMaxHistoryEntries);
  }
  if (options.passwords) {
    PasswordDecryptor decrypt =
        ChromiumPasswordDecryptor(profile.browser, profile.root);
    if (!decrypt) {
      data.no_password_key = true;
    } else {
      data.logins = ReadChromiumLogins(profile.path.AppendASCII("Login Data"),
                                       decrypt, &data.logins_skipped);
    }
  }
}

void ReadFirefoxProfile(const SourceProfile& profile,
                        const ReadOptions& options,
                        ProfileData& data) {
  std::vector<arc::Item> bookmarks;
  if (options.bookmarks) {
    bookmarks = ReadFirefoxBookmarks(profile.path.AppendASCII("places.sqlite"),
                                     base::StrCat({profile.id, ":"}));
  }
  if (profile.browser == Browser::kZen) {
    std::optional<base::DictValue> state;
    if (options.tabs) {
      state = ReadZenSidebarState(profile.path);
    }
    data.sidebar = ZenSidebar(profile, std::move(bookmarks),
                              state ? &*state : nullptr, options.tabs);
    return;
  }
  std::optional<base::DictValue> session;
  if (options.tabs) {
    session = ReadFirefoxSession(profile.path);
  }
  data.sidebar = FirefoxSidebar(profile, std::move(bookmarks),
                                session ? &*session : nullptr, options.tabs);
}

#if BUILDFLAG(IS_MAC)
void ReadSafariProfile(const SourceProfile& profile,
                       const ReadOptions& options,
                       ProfileData& data) {
  std::vector<arc::Item> bookmarks;
  bool unreadable = false;
  if (options.bookmarks) {
    std::optional<base::DictValue> plist =
        ReadPlist(profile.path.AppendASCII("Bookmarks.plist"));
    if (plist) {
      bookmarks =
          ParseSafariBookmarks(*plist, base::StrCat({profile.id, ":"}));
    } else {
      unreadable = true;
    }
  }
  std::vector<SessionTab> tabs;
  std::vector<SafariTabGroup> groups;
  if (options.tabs) {
    bool have_session = false;
    for (const base::FilePath& path : SafariLastSessionPaths(profile.path)) {
      if (std::optional<base::DictValue> plist = ReadPlist(path)) {
        tabs = ParseSafariLastSession(*plist);
        have_session = true;
        break;
      }
    }
    // Tab groups, and (where Safari keeps no LastSession.plist) the open
    // windows' tabs.
    for (const base::FilePath& path : SafariTabsPaths(profile.path)) {
      if (!base::PathExists(path)) {
        continue;
      }
      if (std::optional<SafariTabs> safari_tabs = ReadSafariTabs(path)) {
        groups = std::move(safari_tabs->groups);
        if (!have_session) {
          tabs = std::move(safari_tabs->open_tabs);
        }
      } else {
        unreadable |= !base::PathIsReadable(path);
      }
      break;
    }
  }
  data.sidebar = SafariSidebar(profile, std::move(bookmarks), tabs, groups);
  if (options.history) {
    const base::FilePath history = profile.path.AppendASCII("History.db");
    data.history = ReadSafariHistory(history, kMaxHistoryEntries);
    unreadable |= data.history.empty() && !base::PathIsReadable(history);
  }
  if (unreadable) {
    data.problems.push_back(
        "Compass needs Full Disk Access to read Safari's bookmarks, tab "
        "groups and history (System Settings, Privacy & Security, Full Disk "
        "Access).");
  }
}
#endif

}  // namespace

ProfileData::ProfileData() = default;
ProfileData::ProfileData(ProfileData&&) = default;
ProfileData& ProfileData::operator=(ProfileData&&) = default;
ProfileData::~ProfileData() = default;

ProfileData ReadProfile(const SourceProfile& profile,
                        const ReadOptions& options) {
  ProfileData data;
  if (IsChromiumBased(profile.browser)) {
    ReadChromiumProfile(profile, options, data);
  } else if (IsFirefoxBased(profile.browser)) {
    ReadFirefoxProfile(profile, options, data);
  }
#if BUILDFLAG(IS_MAC)
  if (profile.browser == Browser::kSafari) {
    ReadSafariProfile(profile, options, data);
  }
#endif
  return data;
}

bool CanImportHistory(Browser browser) {
  return true;
}

bool CanImportPasswords(Browser browser) {
  if (IsFirefoxBased(browser)) {
    // Chromium's Firefox importer decrypts them with the browser's NSS,
    // which it doesn't load on macOS.
#if BUILDFLAG(IS_MAC)
    return false;
#else
    return true;
#endif
  }
  // Safari's are in the Keychain (or iCloud Keychain), which Compass can't
  // read; Safari exports them to a file instead.
  return IsChromiumBased(browser);
}

bool ImportsHistoryThroughChromium(Browser browser) {
  return IsFirefoxBased(browser);
}

}  // namespace compass::browser_import
