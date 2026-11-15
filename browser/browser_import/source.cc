// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/source.h"

#include <utility>

#include "base/files/file_util.h"
#include "base/strings/strcat.h"

namespace compass::browser_import {

std::u16string BrowserName(Browser browser) {
  switch (browser) {
    case Browser::kChrome:
      return u"Chrome";
    case Browser::kEdge:
      return u"Edge";
    case Browser::kBrave:
      return u"Brave";
    case Browser::kVivaldi:
      return u"Vivaldi";
    case Browser::kFirefox:
      return u"Firefox";
    case Browser::kZen:
      return u"Zen";
  }
}

std::string_view BrowserKey(Browser browser) {
  switch (browser) {
    case Browser::kChrome:
      return "chrome";
    case Browser::kEdge:
      return "edge";
    case Browser::kBrave:
      return "brave";
    case Browser::kVivaldi:
      return "vivaldi";
    case Browser::kFirefox:
      return "firefox";
    case Browser::kZen:
      return "zen";
  }
}

std::optional<Browser> BrowserFromKey(std::string_view key) {
  for (Browser browser : kAllBrowsers) {
    if (BrowserKey(browser) == key) {
      return browser;
    }
  }
  return std::nullopt;
}

bool IsChromiumBased(Browser browser) {
  switch (browser) {
    case Browser::kChrome:
    case Browser::kEdge:
    case Browser::kBrave:
    case Browser::kVivaldi:
      return true;
    case Browser::kFirefox:
    case Browser::kZen:
      return false;
  }
}

bool IsFirefoxBased(Browser browser) {
  return browser == Browser::kFirefox || browser == Browser::kZen;
}

SourceProfile::SourceProfile() = default;
SourceProfile::SourceProfile(const SourceProfile&) = default;
SourceProfile& SourceProfile::operator=(const SourceProfile&) = default;
SourceProfile::~SourceProfile() = default;

std::u16string SpaceNameFor(const SourceProfile& profile) {
  const std::u16string browser = BrowserName(profile.browser);
  if (!profile.several || profile.name.empty()) {
    return browser;
  }
  return base::StrCat({browser, u" – ", profile.name});
}

ImportedSidebar::ImportedSidebar() = default;
ImportedSidebar::ImportedSidebar(const ImportedSidebar&) = default;
ImportedSidebar::ImportedSidebar(ImportedSidebar&&) = default;
ImportedSidebar& ImportedSidebar::operator=(const ImportedSidebar&) = default;
ImportedSidebar& ImportedSidebar::operator=(ImportedSidebar&&) = default;
ImportedSidebar::~ImportedSidebar() = default;

LoginEntry::LoginEntry() = default;
LoginEntry::LoginEntry(const LoginEntry&) = default;
LoginEntry& LoginEntry::operator=(const LoginEntry&) = default;
LoginEntry::~LoginEntry() = default;

int CountTabs(const std::vector<arc::Item>& items) {
  int count = 0;
  for (const arc::Item& item : items) {
    count += item.kind == arc::Item::Kind::kTab ? 1 : CountTabs(item.children);
  }
  return count;
}

arc::Item MakeTab(std::string id,
                  const GURL& url,
                  std::u16string title,
                  base::Time last_active) {
  arc::Item item;
  item.kind = arc::Item::Kind::kTab;
  item.id = std::move(id);
  item.url = url;
  item.saved_title = std::move(title);
  item.last_active = last_active;
  return item;
}

arc::Item MakeFolder(std::string id,
                     std::u16string title,
                     std::vector<arc::Item> children) {
  arc::Item item;
  item.kind = arc::Item::Kind::kFolder;
  item.id = std::move(id);
  item.title = std::move(title);
  item.children = std::move(children);
  return item;
}

base::FilePath CopyDatabase(const base::FilePath& from,
                            const base::FilePath& dir) {
  const base::FilePath to = dir.Append(from.BaseName());
  if (!base::CopyFile(from, to)) {
    return base::FilePath();
  }
  for (const char* suffix : {"-wal", "-journal"}) {
    const base::FilePath::StringType tail =
        base::FilePath::FromASCII(suffix).value();
    const base::FilePath side(from.value() + tail);
    if (base::PathExists(side)) {
      base::CopyFile(side, base::FilePath(to.value() + tail));
    }
  }
  return to;
}

}  // namespace compass::browser_import
