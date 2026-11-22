// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/detect.h"

#include <initializer_list>
#include <set>
#include <string>
#include <utility>

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "compass/browser/browser_import/chromium_reader.h"
#include "compass/browser/browser_import/firefox_reader.h"

namespace compass::browser_import {

namespace {

std::optional<std::vector<BrowserRoot>>& RootsForTesting() {
  static base::NoDestructor<std::optional<std::vector<BrowserRoot>>> roots;
  return *roots;
}

base::FilePath Join(base::FilePath base, std::initializer_list<const char*> parts) {
  for (const char* part : parts) {
    base = base.AppendASCII(part);
  }
  return base;
}

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

void AddChromiumProfiles(Browser browser,
                         const base::FilePath& root,
                         std::vector<SourceProfile>& out) {
  std::optional<base::DictValue> local_state =
      ReadJsonDict(root.AppendASCII("Local State"));
  if (!local_state) {
    return;
  }
  std::vector<SourceProfile> found;
  for (const ChromiumProfileInfo& info : ParseChromiumProfiles(*local_state)) {
    const base::FilePath path = root.AppendASCII(info.dir);
    if (!base::DirectoryExists(path)) {
      continue;
    }
    SourceProfile profile;
    profile.browser = browser;
    profile.path = path;
    profile.root = root;
    profile.name = info.name;
    profile.id = base::StrCat({BrowserKey(browser), ":", info.dir});
    found.push_back(std::move(profile));
  }
  for (SourceProfile& profile : found) {
    profile.several = found.size() > 1;
    out.push_back(std::move(profile));
  }
}

void AddFirefoxProfiles(Browser browser,
                        const base::FilePath& root,
                        std::vector<SourceProfile>& out) {
  std::string ini;
  if (!base::ReadFileToString(root.AppendASCII("profiles.ini"), &ini)) {
    return;
  }
  std::vector<SourceProfile> found;
  for (const FirefoxProfileInfo& info : ParseProfilesIni(ini, root)) {
    if (!base::DirectoryExists(info.path)) {
      continue;
    }
    SourceProfile profile;
    profile.browser = browser;
    profile.path = info.path;
    profile.root = root;
    profile.app_path = FirefoxAppPath(info.path);
    profile.name = info.name;
    profile.id = base::StrCat(
        {BrowserKey(browser), ":", info.path.BaseName().AsUTF8Unsafe()});
    found.push_back(std::move(profile));
  }
  for (SourceProfile& profile : found) {
    profile.several = found.size() > 1;
    out.push_back(std::move(profile));
  }
}

// Safari has one profile Compass reads, its default one.
void AddSafariProfile(const base::FilePath& root,
                      std::vector<SourceProfile>& out) {
  SourceProfile profile;
  profile.browser = Browser::kSafari;
  profile.path = root;
  profile.root = root;
  profile.id = base::StrCat({BrowserKey(Browser::kSafari), ":Default"});
  out.push_back(std::move(profile));
}

}  // namespace

std::vector<BrowserRoot> CandidateRoots() {
  if (RootsForTesting()) {
    return *RootsForTesting();
  }
  std::vector<BrowserRoot> roots;
#if BUILDFLAG(IS_WIN)
  base::FilePath local;
  if (base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local)) {
    roots.push_back(
        {Browser::kChrome, Join(local, {"Google", "Chrome", "User Data"})});
    roots.push_back(
        {Browser::kEdge, Join(local, {"Microsoft", "Edge", "User Data"})});
    roots.push_back(
        {Browser::kBrave,
         Join(local, {"BraveSoftware", "Brave-Browser", "User Data"})});
    roots.push_back({Browser::kVivaldi, Join(local, {"Vivaldi", "User Data"})});
    roots.push_back({Browser::kDia, Join(local, {"Dia", "User Data"})});
    roots.push_back({Browser::kDia,
                     Join(local, {"TheBrowserCompany", "Dia", "User Data"})});
  }
  base::FilePath roaming;
  if (base::PathService::Get(base::DIR_ROAMING_APP_DATA, &roaming)) {
    roots.push_back({Browser::kFirefox, Join(roaming, {"Mozilla", "Firefox"})});
    roots.push_back({Browser::kZen, Join(roaming, {"zen"})});
  }
#elif BUILDFLAG(IS_MAC)
  base::FilePath support;  // ~/Library/Application Support
  if (base::PathService::Get(base::DIR_APP_DATA, &support)) {
    roots.push_back({Browser::kChrome, Join(support, {"Google", "Chrome"})});
    roots.push_back({Browser::kEdge, Join(support, {"Microsoft Edge"})});
    roots.push_back(
        {Browser::kBrave, Join(support, {"BraveSoftware", "Brave-Browser"})});
    roots.push_back({Browser::kVivaldi, Join(support, {"Vivaldi"})});
    roots.push_back({Browser::kDia, Join(support, {"Dia", "User Data"})});
    roots.push_back({Browser::kFirefox, Join(support, {"Firefox"})});
    roots.push_back({Browser::kZen, Join(support, {"zen"})});
  }
  roots.push_back(
      {Browser::kSafari, Join(base::GetHomeDir(), {"Library", "Safari"})});
#elif BUILDFLAG(IS_LINUX)
  const base::FilePath home = base::GetHomeDir();
  const base::FilePath config = home.AppendASCII(".config");
  // Distribution packages, then Flatpak's.
  roots.push_back({Browser::kChrome, Join(config, {"google-chrome"})});
  roots.push_back(
      {Browser::kChrome,
       Join(home, {".var", "app", "com.google.Chrome", "config",
                   "google-chrome"})});
  roots.push_back({Browser::kEdge, Join(config, {"microsoft-edge"})});
  roots.push_back(
      {Browser::kBrave, Join(config, {"BraveSoftware", "Brave-Browser"})});
  roots.push_back(
      {Browser::kBrave,
       Join(home, {".var", "app", "com.brave.Browser", "config",
                   "BraveSoftware", "Brave-Browser"})});
  roots.push_back({Browser::kVivaldi, Join(config, {"vivaldi"})});
  roots.push_back({Browser::kFirefox, Join(home, {".mozilla", "firefox"})});
  roots.push_back({Browser::kFirefox, Join(config, {"mozilla", "firefox"})});
  roots.push_back(
      {Browser::kFirefox,
       Join(home, {"snap", "firefox", "common", ".mozilla", "firefox"})});
  roots.push_back({Browser::kFirefox,
                   Join(home, {".var", "app", "org.mozilla.firefox",
                               ".mozilla", "firefox"})});
  roots.push_back({Browser::kZen, Join(home, {".zen"})});
  roots.push_back({Browser::kZen, Join(config, {"zen"})});
  roots.push_back(
      {Browser::kZen, Join(home, {".var", "app", "app.zen_browser.zen", ".zen"})});
#endif
  return roots;
}

void SetRootsForTesting(std::optional<std::vector<BrowserRoot>> roots) {
  RootsForTesting() = std::move(roots);
}

std::vector<SourceProfile> DetectProfiles() {
  std::vector<SourceProfile> profiles;
  std::set<Browser> found;
  for (const BrowserRoot& root : CandidateRoots()) {
    if (found.contains(root.browser) || !base::DirectoryExists(root.root)) {
      continue;
    }
    const size_t before = profiles.size();
    if (IsChromiumBased(root.browser)) {
      AddChromiumProfiles(root.browser, root.root, profiles);
    } else if (IsFirefoxBased(root.browser)) {
      AddFirefoxProfiles(root.browser, root.root, profiles);
    } else if (root.browser == Browser::kSafari) {
      AddSafariProfile(root.root, profiles);
    }
    if (profiles.size() > before) {
      found.insert(root.browser);
    }
  }
  return profiles;
}

std::optional<int> ChromiumMajorVersion(const base::FilePath& user_data) {
  std::string contents;
  // A version string; anything longer isn't one.
  if (!base::ReadFileToStringWithMaxSize(user_data.AppendASCII("Last Version"),
                                         &contents, 64)) {
    return std::nullopt;
  }
  const std::string_view version =
      base::TrimWhitespaceASCII(contents, base::TRIM_ALL);
  int major = 0;
  if (!base::StringToInt(version.substr(0, version.find('.')), &major) ||
      major <= 0) {
    return std::nullopt;
  }
  return major;
}

bool IsRunning(const SourceProfile& profile) {
  if (IsChromiumBased(profile.browser)) {
    return IsChromiumRunning(profile.root);
  }
  if (IsFirefoxBased(profile.browser)) {
    return IsFirefoxProfileInUse(profile.path);
  }
  return false;
}

}  // namespace compass::browser_import
