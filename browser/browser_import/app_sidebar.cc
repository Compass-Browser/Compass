// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/app_sidebar.h"

#include <algorithm>
#include <map>
#include <utility>

#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "compass/browser/sidebar/sidebar_serializer.h"
#include "compass/strings/grit/compass_strings.h"
#include "ui/base/l10n/l10n_util.h"

namespace compass::browser_import {

namespace {

// |text|'s digits at [from, from + count) as a number, or nullopt.
std::optional<int> DigitsAt(std::string_view text, size_t from, size_t count) {
  if (from + count > text.size()) {
    return std::nullopt;
  }
  int value = 0;
  for (size_t i = from; i < from + count; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return std::nullopt;
    }
    value = value * 10 + (text[i] - '0');
  }
  return value;
}

}  // namespace

std::optional<base::Value> ReadJsonFile(const base::FilePath& path) {
  std::string json;
  if (!base::ReadFileToString(path, &json)) {
    return std::nullopt;
  }
  return ParseJson(json);
}

std::optional<base::Value> ParseJson(std::string_view json) {
  return base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
}

std::string StringAt(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

std::optional<SpaceColor> HexSpaceColor(std::string_view hex) {
  if (hex.starts_with('#')) {
    hex.remove_prefix(1);
  }
  if (hex.size() != 6 && hex.size() != 8) {
    return std::nullopt;
  }
  uint32_t rgb = 0;
  if (!base::HexStringToUInt(hex.substr(0, 6), &rgb)) {
    return std::nullopt;
  }
  return arc::ArcSpaceColor(((rgb >> 16) & 0xff) / 255.0,
                            ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0);
}

SpaceIcon AppSpaceIcon(std::string_view emoji, std::string_view symbol) {
  // An emoji, not a symbol's name: Yab keeps either in one field.
  if (!emoji.empty() && !base::IsStringASCII(emoji)) {
    if (std::optional<SpaceIcon> icon = SpaceIcon::Emoji(emoji)) {
      return *icon;
    }
  }
  if (!symbol.empty()) {
    for (std::string_view name : {symbol, symbol.substr(0, symbol.find('.'))}) {
      if (std::optional<std::string_view> compass =
              arc::ArcSymbolToCompass(name)) {
        if (std::optional<SpaceIcon> icon = SpaceIcon::Symbol(*compass)) {
          return *icon;
        }
      }
    }
  }
  return SpaceIcon();
}

base::Time TimeFromIso8601(std::string_view text) {
  // YYYY-MM-DDTHH:MM:SS, then optional fractions, then Z or ±HH:MM.
  const std::optional<int> year = DigitsAt(text, 0, 4);
  const std::optional<int> month = DigitsAt(text, 5, 2);
  const std::optional<int> day = DigitsAt(text, 8, 2);
  const std::optional<int> hour = DigitsAt(text, 11, 2);
  const std::optional<int> minute = DigitsAt(text, 14, 2);
  const std::optional<int> second = DigitsAt(text, 17, 2);
  if (!year || !month || !day || !hour || !minute || !second ||
      text[4] != '-' || text[7] != '-' ||
      (text[10] != 'T' && text[10] != ' ') || text[13] != ':' ||
      text[16] != ':') {
    return base::Time();
  }
  size_t at = 19;
  if (at < text.size() && text[at] == '.') {
    ++at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
      ++at;
    }
  }
  int offset_minutes = 0;
  if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
    const std::optional<int> offset_hours = DigitsAt(text, at + 1, 2);
    const std::optional<int> offset_mins = DigitsAt(text, at + 4, 2);
    if (!offset_hours || !offset_mins) {
      return base::Time();
    }
    offset_minutes =
        (*offset_hours * 60 + *offset_mins) * (text[at] == '-' ? -1 : 1);
  }
  base::Time::Exploded exploded = {};
  exploded.year = *year;
  exploded.month = *month;
  exploded.day_of_month = *day;
  exploded.hour = *hour;
  exploded.minute = *minute;
  exploded.second = *second;
  base::Time time;
  if (!base::Time::FromUTCExploded(exploded, &time)) {
    return base::Time();
  }
  return time - base::Minutes(offset_minutes);
}

bool IsImportableTabURL(const GURL& url) {
  return IsRestorableURL(url) && !url.SchemeIs("about") &&
         !url.SchemeIs("javascript");
}

std::vector<HistoryEntry> MergeHistory(std::vector<HistoryEntry> entries,
                                       size_t max_entries) {
  std::map<GURL, HistoryEntry> by_url;
  for (HistoryEntry& entry : entries) {
    auto [it, added] = by_url.try_emplace(entry.url, entry);
    if (added) {
      continue;
    }
    HistoryEntry& kept = it->second;
    kept.visit_count += entry.visit_count;
    kept.typed_count += entry.typed_count;
    if (entry.last_visit > kept.last_visit) {
      kept.last_visit = entry.last_visit;
      if (!entry.title.empty()) {
        kept.title = std::move(entry.title);
      }
    }
  }
  std::vector<HistoryEntry> merged;
  merged.reserve(by_url.size());
  for (auto& [url, entry] : by_url) {
    merged.push_back(std::move(entry));
  }
  std::sort(merged.begin(), merged.end(),
            [](const HistoryEntry& a, const HistoryEntry& b) {
              return a.last_visit > b.last_visit;
            });
  if (merged.size() > max_entries) {
    merged.resize(max_entries);
  }
  return merged;
}

struct FolderTreeBuilder::Node {
  Node() = default;
  Node(Node&&) = default;
  Node& operator=(Node&&) = default;
  ~Node() = default;

  // A folder's own (children aside), or an entry.
  arc::Item item;
  bool folder = false;
  std::vector<Node> children;
  // Folders among |children|, by the browser's ID.
  std::map<std::string, size_t> folders;

  std::vector<arc::Item> TakeChildren() {
    std::vector<arc::Item> items;
    for (Node& child : children) {
      if (!child.folder) {
        items.push_back(std::move(child.item));
        continue;
      }
      std::vector<arc::Item> grandchildren = child.TakeChildren();
      if (grandchildren.empty()) {
        continue;
      }
      child.item.children = std::move(grandchildren);
      items.push_back(std::move(child.item));
    }
    return items;
  }
};

FolderTreeBuilder::FolderTreeBuilder(std::string id_prefix)
    : id_prefix_(std::move(id_prefix)), root_(std::make_unique<Node>()) {}

FolderTreeBuilder::~FolderTreeBuilder() = default;

void FolderTreeBuilder::Add(const std::vector<FolderStep>& path,
                            arc::Item item) {
  Node* node = root_.get();
  for (const FolderStep& step : path) {
    auto [it, added] =
        node->folders.try_emplace(step.id, node->children.size());
    if (added) {
      Node folder;
      folder.folder = true;
      folder.item =
          MakeFolder(base::StrCat({id_prefix_, "folder:", step.id}),
                     step.name.empty()
                         ? l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_FOLDER)
                         : step.name,
                     {});
      node->children.push_back(std::move(folder));
    }
    node = &node->children[it->second];
  }
  Node leaf;
  leaf.item = std::move(item);
  node->children.push_back(std::move(leaf));
}

std::vector<arc::Item> FolderTreeBuilder::Build() && {
  return root_->TakeChildren();
}

AppProfile::AppProfile() = default;
AppProfile::AppProfile(const AppProfile&) = default;
AppProfile& AppProfile::operator=(const AppProfile&) = default;
AppProfile::~AppProfile() = default;

std::u16string AppProfileName(const std::vector<std::u16string>& spaces) {
  constexpr size_t kMaxNames = 3;
  std::u16string name;
  for (size_t i = 0; i < spaces.size() && i < kMaxNames; ++i) {
    if (i) {
      name += u", ";
    }
    name += spaces[i];
  }
  if (spaces.size() > kMaxNames) {
    name += u"…";
  }
  return name;
}

void AddBookmarksSpace(const SourceProfile& profile,
                       std::vector<arc::Item> bookmarks,
                       ImportedSidebar& sidebar) {
  if (bookmarks.empty() && !sidebar.spaces.empty()) {
    return;
  }
  arc::Space space;
  space.id = base::StrCat({profile.id, ":space"});
  space.name = SpaceNameFor(profile);
  space.pinned = std::move(bookmarks);
  sidebar.spaces.push_back(std::move(space));
}

}  // namespace compass::browser_import
