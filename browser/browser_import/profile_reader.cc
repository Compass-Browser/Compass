// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/profile_reader.h"

#include <optional>
#include <utility>

#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "compass/browser/browser_import/app_sidebar.h"
#include "compass/browser/browser_import/ardia_reader.h"
#include "compass/browser/browser_import/chromium_crypto.h"
#include "compass/browser/browser_import/chromium_reader.h"
#include "compass/browser/browser_import/cra_reader.h"
#include "compass/browser/browser_import/crest_reader.h"
#include "compass/browser/browser_import/firefox_reader.h"
#include "compass/browser/browser_import/safari_reader.h"
#include "compass/browser/browser_import/yab_reader.h"

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

void ReadChromiumHistoryAndPasswords(const SourceProfile& profile,
                                     const ReadOptions& options,
                                     ProfileData& data);

// Ardia's and Cra's sidebars, from their own files (ardia_reader.h,
// cra_reader.h); nullopt for other browsers, or an Ardia from before its
// session.json, whose Chromium session files are read instead.
std::optional<ImportedSidebar> ReadAppSidebar(
    const SourceProfile& profile,
    const ReadOptions& options,
    const std::optional<base::DictValue>& bookmarks) {
  std::vector<base::FilePath> paths;
  if (profile.browser == Browser::kArdia) {
    paths = ArdiaSessionPaths(profile);
  } else if (profile.browser == Browser::kCra) {
    paths = {CraSidebarPath(profile.root)};
  } else {
    return std::nullopt;
  }
  for (const base::FilePath& path : paths) {
    std::optional<base::Value> document = ReadJsonFile(path);
    if (!document || !document->is_dict()) {
      continue;
    }
    std::vector<arc::Item> pinned;
    if (options.bookmarks && bookmarks) {
      pinned = ParseChromiumBookmarks(*bookmarks,
                                      base::StrCat({profile.id, ":"}));
    }
    return profile.browser == Browser::kArdia
               ? ArdiaSidebar(profile, document->GetDict(), std::move(pinned),
                              options.tabs)
               : CraSidebar(profile, document->GetDict(), std::move(pinned),
                            options.tabs);
  }
  return std::nullopt;
}

void ReadChromiumProfile(const SourceProfile& profile,
                         const ReadOptions& options,
                         ProfileData& data) {
  std::optional<base::DictValue> bookmarks;
  if (options.bookmarks) {
    bookmarks = ReadJsonDict(profile.path.AppendASCII("Bookmarks"));
  }
  if (std::optional<ImportedSidebar> sidebar =
          ReadAppSidebar(profile, options, bookmarks)) {
    data.sidebar = std::move(*sidebar);
    ReadChromiumHistoryAndPasswords(profile, options, data);
    return;
  }
  std::optional<base::DictValue> preferences;
  std::vector<std::vector<uint8_t>> session_files;
  if (options.tabs) {
    session_files = ReadChromiumSessionFiles(profile.path);
    if (profile.browser == Browser::kVivaldi) {
      preferences = ReadJsonDict(profile.path.AppendASCII("Preferences"));
    }
  }
  // Built by FinishReadingProfile(), on the UI thread.
  UnbuiltChromiumSidebar& unbuilt = data.unbuilt.emplace();
  unbuilt.profile = profile;
  unbuilt.options = options;
  unbuilt.bookmarks = std::move(bookmarks);
  unbuilt.preferences = std::move(preferences);
  unbuilt.session_files = std::move(session_files);
  ReadChromiumHistoryAndPasswords(profile, options, data);
}

void ReadChromiumHistoryAndPasswords(const SourceProfile& profile,
                                     const ReadOptions& options,
                                     ProfileData& data) {
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

void ReadCrestProfile(const SourceProfile& profile,
                      const ReadOptions& options,
                      ProfileData& data) {
  // Its root is Crest's Chromium folder, inside Crest's own.
  std::optional<CrestSession> session = ReadCrestSession(
      CrestSessionPath(profile.root.DirName()), options.history);
  if (!session) {
    data.problems.push_back(
        "Crest's spaces couldn't be read. Open Crest once, quit it, then "
        "try again.");
    return;
  }
  if (options.tabs) {
    data.sidebar = CrestSidebar(profile, *session->core);
  }
  if (options.history) {
    std::vector<HistoryEntry> all;
    for (const std::string& space :
         CrestSpaceIds(*session->core, SourceKey(profile))) {
      const auto rows = session->history.find(base::ToUpperASCII(space));
      if (rows == session->history.end()) {
        continue;
      }
      std::vector<HistoryEntry> entries = ParseCrestHistory(rows->second);
      all.insert(all.end(), std::make_move_iterator(entries.begin()),
                 std::make_move_iterator(entries.end()));
    }
    data.history = MergeHistory(std::move(all), kMaxHistoryEntries);
  }
}

void ReadYabProfile(const SourceProfile& profile,
                    const ReadOptions& options,
                    ProfileData& data) {
  data.sidebar = ReadYabSidebar(profile, options.bookmarks, options.tabs);
  if (options.history) {
    data.history = ReadYabHistory(profile.root.DirName(), SourceKey(profile),
                                  kMaxHistoryEntries);
  }
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

UnbuiltChromiumSidebar::UnbuiltChromiumSidebar() = default;
UnbuiltChromiumSidebar::UnbuiltChromiumSidebar(UnbuiltChromiumSidebar&&) =
    default;
UnbuiltChromiumSidebar& UnbuiltChromiumSidebar::operator=(
    UnbuiltChromiumSidebar&&) = default;
UnbuiltChromiumSidebar::~UnbuiltChromiumSidebar() = default;

void FinishReadingProfile(ProfileData& data) {
  if (!data.unbuilt) {
    return;
  }
  const UnbuiltChromiumSidebar& unbuilt = *data.unbuilt;
  const std::vector<SessionTab> tabs =
      ParseChromiumSessionFiles(unbuilt.session_files);
  data.sidebar = ChromiumSidebar(
      unbuilt.profile, unbuilt.bookmarks ? &*unbuilt.bookmarks : nullptr,
      unbuilt.preferences ? &*unbuilt.preferences : nullptr, tabs,
      unbuilt.options.bookmarks, unbuilt.options.tabs);
  data.unbuilt.reset();
}

ProfileData ReadProfile(const SourceProfile& profile,
                        const ReadOptions& options) {
  ProfileData data;
  if (IsChromiumBased(profile.browser)) {
    ReadChromiumProfile(profile, options, data);
  } else if (IsFirefoxBased(profile.browser)) {
    ReadFirefoxProfile(profile, options, data);
  } else if (profile.browser == Browser::kCrest) {
    ReadCrestProfile(profile, options, data);
  } else if (profile.browser == Browser::kYab) {
    ReadYabProfile(profile, options, data);
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
  // read; Safari exports them to a file instead. Crest's are in the
  // Keychain under an access group only Crest has, Yab's in the Keychain
  // item by item.
  return IsChromiumBased(browser);
}

bool KeepsHistoryOutsideProfile(Browser browser) {
  return browser == Browser::kCrest || browser == Browser::kYab;
}

bool ImportsHistoryThroughChromium(Browser browser) {
  return IsFirefoxBased(browser);
}

}  // namespace compass::browser_import
