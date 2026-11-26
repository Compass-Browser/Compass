// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/safari_reader.h"

#include <algorithm>
#include <map>
#include <utility>

#include "base/files/scoped_temp_dir.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "compass/browser/sidebar/sidebar_serializer.h"
#include "compass/strings/grit/compass_strings.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "ui/base/l10n/l10n_util.h"

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
      items.push_back(MakeFolder(
          id,
          title.empty() ? l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_FOLDER)
                        : title,
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

// SafariTabs.db's bookmarks table holds tabs and tab groups as bookmarks
// and folders. These are the values Safari's own queries use.
constexpr int kTypeLeaf = 0;
constexpr int kTypeFolder = 1;
// Of a folder: a tab group, a profile, or a list of tab groups.
constexpr int kSubtypeTabGroup = 0;
constexpr int kSubtypeProfile = 2;
constexpr int kSubtypeGroupList = 3;

// A row of SafariTabs.db's bookmarks table.
struct TabsRow {
  int64_t id = 0;
  int type = kTypeLeaf;
  int subtype = 0;
  int special_id = 0;
  bool hidden = false;
  std::string uuid;
  std::u16string title;
  std::string url;
  // Its attributes carry a pinned tab's address.
  bool pinned = false;
};

class SafariTabsTree {
 public:
  // Reads the rows of |db|'s bookmarks table not marked deleted; false if
  // it has none.
  bool Read(sql::Database& db) {
    // Safari's own lists (pinned, recentlyClosed, the windows' unnamed
    // groups) have no parent; tab groups and profiles have Root (0).
    sql::Statement statement(db.GetUniqueStatement(
        "SELECT id, IFNULL(parent, -1), type, subtype, special_id, hidden, "
        "external_uuid, title, url, "
        "IFNULL(INSTR(extra_attributes, CAST('PinnedAddress' AS BLOB)), 0) "
        "+ IFNULL(INSTR(local_attributes, CAST('PinnedAddress' AS BLOB)), 0) "
        "FROM bookmarks WHERE deleted = 0 ORDER BY order_index, id"));
    while (statement.Step()) {
      TabsRow row;
      row.id = statement.ColumnInt64(0);
      const int64_t parent = statement.ColumnInt64(1);
      row.type = statement.ColumnInt(2);
      row.subtype = statement.ColumnInt(3);
      row.special_id = statement.ColumnInt(4);
      row.hidden = statement.ColumnBool(5);
      row.uuid = statement.ColumnString(6);
      row.title = statement.ColumnString16(7);
      row.url = statement.ColumnString(8);
      row.pinned = statement.ColumnInt64(9) > 0;
      if (parent >= 0 && parent != row.id) {
        children_[parent].push_back(rows_.size());
      }
      rows_.push_back(std::move(row));
    }
    return statement.Succeeded() && !rows_.empty();
  }

  // The rows under |parent|, in Safari's order.
  std::vector<const TabsRow*> Children(int64_t parent) const {
    std::vector<const TabsRow*> rows;
    if (const auto it = children_.find(parent); it != children_.end()) {
      for (size_t index : it->second) {
        rows.push_back(&rows_[index]);
      }
    }
    return rows;
  }

  // The tabs under |parent|, the pinned ones first.
  std::vector<SessionTab> Tabs(int64_t parent) const {
    std::vector<SessionTab> tabs;
    for (const TabsRow* row : Children(parent)) {
      if (row->type != kTypeLeaf) {
        continue;
      }
      SessionTab tab;
      tab.url = GURL(row->url);
      tab.title = row->title;
      tab.pinned = row->pinned;
      tabs.push_back(std::move(tab));
    }
    std::stable_partition(tabs.begin(), tabs.end(),
                          [](const SessionTab& tab) { return tab.pinned; });
    return tabs;
  }

  // The named tab groups under |parent| (Root, a profile, or a list of
  // groups within one), in order, into |groups|.
  void AddGroups(int64_t parent,
                 const std::u16string& profile,
                 std::vector<SafariTabGroup>& groups,
                 int depth = 0) const {
    if (depth > 4) {
      return;
    }
    for (const TabsRow* row : Children(parent)) {
      if (row->type != kTypeFolder || row->special_id != 0) {
        continue;
      }
      if (row->subtype == kSubtypeGroupList) {
        AddGroups(row->id, profile, groups, depth + 1);
        continue;
      }
      if (row->subtype != kSubtypeTabGroup || row->hidden) {
        continue;
      }
      SafariTabGroup group;
      group.id =
          row->uuid.empty() ? base::NumberToString(row->id) : row->uuid;
      group.name = row->title;
      group.profile = profile;
      group.tabs = Tabs(row->id);
      // The group's start page favorites: its "TopScopedBookmarkList"
      // folder.
      for (const TabsRow* child : Children(row->id)) {
        if (child->type == kTypeFolder &&
            base::StartsWith(child->title, u"TopScopedBookmarkList")) {
          for (SessionTab& favorite : Tabs(child->id)) {
            favorite.pinned = false;
            group.favorites.push_back(std::move(favorite));
          }
        }
      }
      groups.push_back(std::move(group));
    }
  }

  // Every profile's tab groups, the default profile's first.
  std::vector<SafariTabGroup> Groups() const {
    std::vector<SafariTabGroup> groups;
    AddGroups(0, std::u16string(), groups);
    std::vector<const TabsRow*> profiles;
    for (const TabsRow* row : Children(0)) {
      if (row->type == kTypeFolder && row->subtype == kSubtypeProfile &&
          row->special_id == 0) {
        profiles.push_back(row);
      }
    }
    // The default profile (no title) first.
    std::stable_partition(
        profiles.begin(), profiles.end(),
        [](const TabsRow* row) { return row->title.empty(); });
    for (const TabsRow* row : profiles) {
      AddGroups(row->id, row->title, groups);
    }
    return groups;
  }

  const TabsRow* FindByUuid(std::string_view uuid) const {
    for (const TabsRow& row : rows_) {
      if (row.uuid == uuid) {
        return &row;
      }
    }
    return nullptr;
  }

 private:
  std::vector<TabsRow> rows_;
  std::map<int64_t, std::vector<size_t>> children_;
};

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
      folders.insert(
          folders.begin(),
          MakeFolder(
              id, l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_BOOKMARKS_MENU),
              std::move(children)));
    } else if (title == "com.apple.ReadingList") {
      reading_list = MakeFolder(
          id, l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_READING_LIST),
          std::move(children));
    } else {
      folders.push_back(MakeFolder(
          id,
          title.empty() ? l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_FOLDER)
                        : base::UTF8ToUTF16(title),
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

SafariTabGroup::SafariTabGroup() = default;
SafariTabGroup::SafariTabGroup(const SafariTabGroup&) = default;
SafariTabGroup& SafariTabGroup::operator=(const SafariTabGroup&) = default;
SafariTabGroup::~SafariTabGroup() = default;

SafariTabs::SafariTabs() = default;
SafariTabs::SafariTabs(const SafariTabs&) = default;
SafariTabs& SafariTabs::operator=(const SafariTabs&) = default;
SafariTabs::~SafariTabs() = default;

std::optional<SafariTabs> ReadSafariTabs(const base::FilePath& path) {
  base::ScopedTempDir temp;
  if (!temp.CreateUniqueTempDir()) {
    return std::nullopt;
  }
  const base::FilePath copy = CopyDatabase(path, temp.GetPath());
  if (copy.empty()) {
    return std::nullopt;
  }
  sql::Database db(sql::DatabaseOptions(), "FirefoxImporter");
  if (!db.Open(copy) || !db.DoesTableExist("bookmarks")) {
    return std::nullopt;
  }
  SafariTabsTree tree;
  if (!tree.Read(db)) {
    return std::nullopt;
  }
  SafariTabs tabs;
  tabs.groups = tree.Groups();

  // Safari's pinned tabs (shown in every window outside tab groups), then
  // each open window's own tabs: its unnamed group.
  if (const TabsRow* pinned = tree.FindByUuid("pinned")) {
    for (SessionTab& tab : tree.Tabs(pinned->id)) {
      tab.pinned = true;
      tabs.open_tabs.push_back(std::move(tab));
    }
  }
  if (db.DoesTableExist("windows")) {
    sql::Statement windows(db.GetUniqueStatement(
        "SELECT local_tab_group_id FROM windows "
        "WHERE local_tab_group_id IS NOT NULL "
        "AND (date_closed IS NULL OR is_last_session = 1) ORDER BY id"));
    while (windows.Step()) {
      for (SessionTab& tab : tree.Tabs(windows.ColumnInt64(0))) {
        tab.pinned = false;
        tabs.open_tabs.push_back(std::move(tab));
      }
    }
  }
  return tabs;
}

std::vector<base::FilePath> SafariTabsPaths(const base::FilePath& safari_dir) {
  return {safari_dir.DirName()
              .AppendASCII("Containers")
              .AppendASCII("com.apple.Safari")
              .AppendASCII("Data")
              .AppendASCII("Library")
              .AppendASCII("Safari")
              .AppendASCII("SafariTabs.db"),
          safari_dir.AppendASCII("SafariTabs.db")};
}

ImportedSidebar SafariSidebar(const SourceProfile& profile,
                              std::vector<arc::Item> bookmarks,
                              const std::vector<SessionTab>& tabs,
                              const std::vector<SafariTabGroup>& groups) {
  ImportedSidebar sidebar;
  const std::string prefix = base::StrCat({profile.id, ":"});
  arc::Space space;
  space.id = base::StrCat({prefix, "space"});
  space.name = SpaceNameFor(profile);
  space.pinned = std::move(bookmarks);
  space.today = TodayFromTabs(tabs, base::StrCat({prefix, "today:"}));
  sidebar.spaces.push_back(std::move(space));

  for (const SafariTabGroup& group : groups) {
    const std::string group_prefix =
        base::StrCat({prefix, "tabgroup:", group.id, ":"});
    arc::Space group_space;
    group_space.id = base::StrCat({prefix, "tabgroup:", group.id});
    group_space.name =
        group.name.empty()
            ? l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_TAB_GROUP)
            : group.name;
    if (!group.profile.empty()) {
      group_space.name =
          base::StrCat({group_space.name, u" – ", group.profile});
    }
    group_space.today = TodayFromTabs(group.tabs, group_prefix);
    for (size_t i = 0; i < group.favorites.size(); ++i) {
      const SessionTab& favorite = group.favorites[i];
      if (!IsRestorableURL(favorite.url) ||
          favorite.url.SchemeIs("javascript")) {
        continue;
      }
      group_space.pinned.push_back(MakeTab(
          base::StrCat({group_prefix, "favorite:", base::NumberToString(i),
                        ":", favorite.url.spec()}),
          favorite.url, favorite.title));
    }
    if (CountTabs(group_space.today) + CountTabs(group_space.pinned) == 0) {
      continue;  // Nothing to bring: an empty group.
    }
    sidebar.spaces.push_back(std::move(group_space));
  }
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
