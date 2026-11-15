// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/safari_reader.h"

#include <utility>

#include "base/files/scoped_temp_dir.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "compass/browser/sidebar/sidebar_serializer.h"
#include "sql/database.h"
#include "sql/statement.h"

namespace compass::browser_import {

namespace {

constexpr std::string_view kList = "WebBookmarkTypeList";
constexpr std::string_view kLeaf = "WebBookmarkTypeLeaf";

std::string StringOr(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

// Safari's times: seconds since 2001-01-01.
base::Time FromSafariTime(double seconds) {
  return base::Time::FromSecondsSinceUnixEpoch(978307200.0 + seconds);
}

std::vector<arc::Item> Children(const base::DictValue& list,
                                std::string_view id_prefix) {
  std::vector<arc::Item> items;
  const base::ListValue* children = list.FindList("Children");
  if (!children) {
    return items;
  }
  for (const base::Value& value : *children) {
    const base::DictValue* node = value.GetIfDict();
    if (!node) {
      continue;
    }
    const std::string type = StringOr(*node, "WebBookmarkType");
    const std::string id =
        base::StrCat({id_prefix, "bookmark:", StringOr(*node, "WebBookmarkUUID")});
    if (type == kList) {
      std::u16string title = base::UTF8ToUTF16(StringOr(*node, "Title"));
      items.push_back(MakeFolder(id, title.empty() ? u"Folder" : title,
                                 Children(*node, id_prefix)));
    } else if (type == kLeaf) {
      const GURL url(StringOr(*node, "URLString"));
      if (!IsRestorableURL(url) || url.SchemeIs("javascript")) {
        continue;
      }
      const std::string* title =
          node->FindStringByDottedPath("URIDictionary.title");
      items.push_back(
          MakeTab(id, url, title ? base::UTF8ToUTF16(*title) : u""));
    }
  }
  return items;
}

}  // namespace

std::vector<arc::Item> ParseSafariBookmarks(const base::DictValue& plist,
                                            std::string_view id_prefix) {
  std::vector<arc::Item> bar;
  std::vector<arc::Item> folders;
  std::optional<arc::Item> reading_list;
  const base::ListValue* roots = plist.FindList("Children");
  if (!roots) {
    return bar;
  }
  for (const base::Value& value : *roots) {
    const base::DictValue* root = value.GetIfDict();
    if (!root || StringOr(*root, "WebBookmarkType") != kList) {
      continue;  // History's proxy, or a bookmark at the top.
    }
    const std::string title = StringOr(*root, "Title");
    const std::string id = base::StrCat(
        {id_prefix, "bookmark:", StringOr(*root, "WebBookmarkUUID")});
    std::vector<arc::Item> children = Children(*root, id_prefix);
    if (title == "BookmarksBar") {
      bar = std::move(children);
    } else if (children.empty()) {
      continue;
    } else if (title == "BookmarksMenu") {
      folders.insert(folders.begin(),
                     MakeFolder(id, u"Bookmarks Menu", std::move(children)));
    } else if (title == "com.apple.ReadingList") {
      reading_list = MakeFolder(id, u"Reading List", std::move(children));
    } else {
      folders.push_back(MakeFolder(
          id, title.empty() ? u"Folder" : base::UTF8ToUTF16(title),
          std::move(children)));
    }
  }
  for (arc::Item& folder : folders) {
    bar.push_back(std::move(folder));
  }
  if (reading_list) {
    bar.push_back(std::move(*reading_list));
  }
  return bar;
}

std::vector<SessionTab> ParseSafariLastSession(const base::DictValue& plist) {
  std::vector<SessionTab> tabs;
  const base::ListValue* windows = plist.FindList("SessionWindows");
  if (!windows) {
    return tabs;
  }
  for (const base::Value& value : *windows) {
    const base::DictValue* window = value.GetIfDict();
    if (!window || window->FindBool("IsPrivateWindow").value_or(false)) {
      continue;
    }
    const base::ListValue* states = window->FindList("TabStates");
    if (!states) {
      continue;
    }
    for (const base::Value& state_value : *states) {
      const base::DictValue* state = state_value.GetIfDict();
      if (!state) {
        continue;
      }
      SessionTab tab;
      tab.url = GURL(StringOr(*state, "TabURL"));
      tab.title = base::UTF8ToUTF16(StringOr(*state, "TabTitle"));
      if (std::optional<double> time = state->FindDouble("LastVisitTime")) {
        tab.last_active = FromSafariTime(*time);
      }
      tabs.push_back(std::move(tab));
    }
  }
  return tabs;
}

std::vector<HistoryEntry> ReadSafariHistory(const base::FilePath& history,
                                            size_t max) {
  std::vector<HistoryEntry> entries;
  base::ScopedTempDir temp;
  if (!temp.CreateUniqueTempDir()) {
    return entries;
  }
  const base::FilePath copy = CopyDatabase(history, temp.GetPath());
  if (copy.empty()) {
    return entries;
  }
  sql::Database db(sql::DatabaseOptions(), "FirefoxImporter");
  if (!db.Open(copy)) {
    return entries;
  }
  // Each page with its latest visit's time and title.
  sql::Statement statement(db.GetUniqueStatement(
      "SELECT i.url, v.title, i.visit_count, MAX(v.visit_time) "
      "FROM history_items i JOIN history_visits v ON v.history_item = i.id "
      "GROUP BY i.id ORDER BY 4 DESC LIMIT ?"));
  statement.BindInt64(0, static_cast<int64_t>(max));
  while (statement.Step()) {
    HistoryEntry entry;
    entry.url = GURL(statement.ColumnStringView(0));
    if (!entry.url.is_valid() || !entry.url.SchemeIsHTTPOrHTTPS()) {
      continue;
    }
    entry.title = statement.ColumnString16(1);
    entry.visit_count = statement.ColumnInt(2);
    entry.last_visit = FromSafariTime(statement.ColumnDouble(3));
    entries.push_back(std::move(entry));
  }
  return entries;
}

ImportedSidebar SafariSidebar(const SourceProfile& profile,
                              std::vector<arc::Item> bookmarks,
                              const std::vector<SessionTab>& tabs) {
  ImportedSidebar sidebar;
  const std::string prefix = base::StrCat({profile.id, ":"});
  arc::Space space;
  space.id = base::StrCat({prefix, "space"});
  space.name = SpaceNameFor(profile);
  space.pinned = std::move(bookmarks);
  space.today = TodayFromTabs(tabs, base::StrCat({prefix, "today:"}));
  sidebar.spaces.push_back(std::move(space));
  return sidebar;
}

std::vector<base::FilePath> SafariLastSessionPaths(
    const base::FilePath& safari_dir) {
  // ~/Library/Safari -> ~/Library/Containers/com.apple.Safari/...
  const base::FilePath library = safari_dir.DirName();
  return {library.AppendASCII("Containers")
              .AppendASCII("com.apple.Safari")
              .AppendASCII("Data")
              .AppendASCII("Library")
              .AppendASCII("Safari")
              .AppendASCII("LastSession.plist"),
          safari_dir.AppendASCII("LastSession.plist")};
}

}  // namespace compass::browser_import
