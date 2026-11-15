// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/profile_reader.h"

#include <optional>
#include <utility>

#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "build/build_config.h"
#include "compass/browser/browser_import/chromium_crypto.h"
#include "compass/browser/browser_import/chromium_reader.h"

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
  }
  return data;
}

bool CanImportHistory(Browser browser) {
  return true;
}

bool CanImportPasswords(Browser browser) {
  return IsChromiumBased(browser);
}

bool ImportsHistoryThroughChromium(Browser browser) {
  return false;
}

}  // namespace compass::browser_import
