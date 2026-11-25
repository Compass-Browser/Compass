// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/test_fixtures.h"

#include <map>
#include <memory>

#include "base/files/file_util.h"
#include "base/json/json_writer.h"
#include "base/pickle.h"
#include "base/strings/strcat.h"
#include "base/time/time.h"
#include "base/values.h"
#include "components/sessions/core/serialized_navigation_entry.h"
#include "components/sessions/core/session_command.h"
#include "components/sessions/core/session_id.h"
#include "components/sessions/core/session_service_commands.h"
#include "components/sessions/core/session_types.h"
#include "components/tab_groups/tab_group_color.h"
#include "components/tab_groups/tab_group_id.h"
#include "components/tab_groups/tab_group_visual_data.h"
#include "sql/database.h"
#include "sql/test/test_helpers.h"
#include "sql/statement.h"
#include "url/gurl.h"

namespace compass::browser_import::test {

namespace {

// A command id no Chromium uses, as Vivaldi's tab data command.
constexpr sessions::SessionCommand::id_type kVivaldiTabDataCommand = 200;

void Append(std::vector<uint8_t>& file,
            std::unique_ptr<sessions::SessionCommand> command) {
  std::vector<uint8_t> bytes = command->Serialize(nullptr);
  file.insert(file.end(), bytes.begin(), bytes.end());
}

int64_t ChromiumNow() {
  return base::Time::Now().ToDeltaSinceWindowsEpoch().InMicroseconds();
}

}  // namespace

std::vector<uint8_t> MakeChromiumSessionFile(
    const std::vector<std::vector<FixtureTab>>& windows) {
  std::vector<uint8_t> file;
  const uint32_t header[] = {0x53534E53, 3};
  const auto header_bytes = base::as_byte_span(header);
  file.insert(file.end(), header_bytes.begin(), header_bytes.end());

  for (const std::vector<FixtureTab>& tabs : windows) {
    const SessionID window = SessionID::NewUnique();
    Append(file, sessions::CreateSetWindowTypeCommand(
                     window, sessions::SessionWindow::TYPE_NORMAL));
    std::map<std::u16string, tab_groups::TabGroupId> groups;
    for (size_t i = 0; i < tabs.size(); ++i) {
      const FixtureTab& fixture = tabs[i];
      const SessionID tab = SessionID::NewUnique();
      Append(file, sessions::CreateSetTabWindowCommand(window, tab));
      Append(file, sessions::CreateSetTabIndexInWindowCommand(
                       tab, static_cast<int>(i)));
      sessions::SerializedNavigationEntry navigation;
      navigation.set_index(0);
      navigation.set_virtual_url(GURL(fixture.url));
      navigation.set_title(fixture.title);
      Append(file, sessions::CreateUpdateTabNavigationCommand(tab, navigation));
      Append(file, sessions::CreateSetSelectedNavigationIndexCommand(tab, 0));
      if (fixture.pinned) {
        Append(file, sessions::CreatePinnedStateCommand(tab, true));
      }
      if (!fixture.group.empty()) {
        auto it = groups.find(fixture.group);
        if (it == groups.end()) {
          it = groups
                   .emplace(fixture.group,
                            tab_groups::TabGroupId::GenerateNew())
                   .first;
          const tab_groups::TabGroupVisualData visual(
              fixture.group, tab_groups::TabGroupColorId::kBlue);
          Append(file, sessions::CreateTabGroupMetadataUpdateCommand(
                           it->second, &visual));
        }
        Append(file, sessions::CreateTabGroupCommand(tab, it->second));
      }
      if (fixture.untitled_group) {
        const tab_groups::TabGroupId group =
            tab_groups::TabGroupId::GenerateNew();
        const tab_groups::TabGroupVisualData visual(
            u"", tab_groups::TabGroupColorId::kGrey);
        Append(file,
               sessions::CreateTabGroupMetadataUpdateCommand(group, &visual));
        Append(file, sessions::CreateTabGroupCommand(tab, group));
      }
      if (!fixture.vivaldi_workspace.empty()) {
        base::Pickle pickle;
        pickle.WriteInt(tab.id());
        pickle.WriteString(base::StrCat(
            {"{\"workspaceId\":", fixture.vivaldi_workspace, "}"}));
        Append(file, std::make_unique<sessions::SessionCommand>(
                         kVivaldiTabDataCommand, pickle));
      }
    }
  }
  return file;
}

std::string MakeChromiumBookmarksJson() {
  return R"json({
  "checksum": "",
  "roots": {
    "bookmark_bar": {
      "children": [
        {"guid": "00000000-0000-4000-8000-000000000001", "id": "1",
         "name": "Bar link", "type": "url", "url": "https://bar.test/"},
        {"guid": "00000000-0000-4000-8000-000000000002", "id": "2",
         "name": "Docs", "type": "folder", "children": [
          {"guid": "00000000-0000-4000-8000-000000000003", "id": "3",
           "name": "Guide", "type": "url",
           "url": "https://docs.test/guide"},
          {"guid": "00000000-0000-4000-8000-000000000004", "id": "4",
           "name": "Script", "type": "url", "url": "javascript:void(0)"}
        ]}
      ],
      "guid": "0bc5d13f-2cba-5d74-951f-3f233fe6c908",
      "id": "10", "name": "Bookmarks bar", "type": "folder"
    },
    "other": {
      "children": [
        {"guid": "00000000-0000-4000-8000-000000000005", "id": "5",
         "name": "Other link", "type": "url", "url": "https://other.test/"}
      ],
      "guid": "82b081ec-3dd3-529c-8475-ab6c344590dd",
      "id": "11", "name": "Other bookmarks", "type": "folder"
    },
    "synced": {
      "children": [],
      "guid": "4cf2e351-0e85-532b-bb37-df045d8f8d0f",
      "id": "12", "name": "Mobile bookmarks", "type": "folder"
    }
  },
  "version": 1
})json";
}

bool WriteChromiumUserData(const base::FilePath& user_data,
                           const std::string& profile_name,
                           const std::vector<FixtureTab>& tabs,
                           const Encrypt& encrypt,
                           const std::string& local_state_os_crypt_key) {
  const base::FilePath profile = user_data.AppendASCII("Default");
  if (!base::CreateDirectory(profile.AppendASCII("Sessions"))) {
    return false;
  }
  base::DictValue local_state;
  local_state.SetByDottedPath("profile.info_cache.Default.name", profile_name);
  if (!local_state_os_crypt_key.empty()) {
    local_state.SetByDottedPath("os_crypt.encrypted_key",
                                local_state_os_crypt_key);
  }
  std::optional<std::string> local_state_json =
      base::WriteJson(base::Value(std::move(local_state)));
  if (!local_state_json ||
      !base::WriteFile(user_data.AppendASCII("Local State"),
                       *local_state_json) ||
      !base::WriteFile(profile.AppendASCII("Bookmarks"),
                       MakeChromiumBookmarksJson()) ||
      !base::WriteFile(
          profile.AppendASCII("Sessions").AppendASCII("Session_13400000000000000"),
          MakeChromiumSessionFile({tabs}))) {
    return false;
  }

  {
    sql::Database db(sql::test::kTestTag);
    if (!db.Open(profile.AppendASCII("History")) ||
        !db.Execute("CREATE TABLE urls (id INTEGER PRIMARY KEY, url TEXT, "
                    "title TEXT, visit_count INTEGER, typed_count INTEGER, "
                    "last_visit_time INTEGER, hidden INTEGER DEFAULT 0)")) {
      return false;
    }
    sql::Statement insert(db.GetUniqueStatement(
        "INSERT INTO urls (url, title, visit_count, typed_count, "
        "last_visit_time) VALUES (?, ?, ?, ?, ?)"));
    int64_t time = ChromiumNow();
    for (const char* page : {"https://history.test/a",
                             "https://history.test/b"}) {
      insert.Reset(true);
      insert.BindString(0, page);
      insert.BindString(1, base::StrCat({"Page ", page}));
      insert.BindInt(2, 3);
      insert.BindInt(3, 1);
      insert.BindInt64(4, time);
      time -= 1000000;
      if (!insert.Run()) {
        return false;
      }
    }
  }
  {
    sql::Database db(sql::test::kTestTag);
    if (!db.Open(profile.AppendASCII("Login Data")) ||
        !db.Execute("CREATE TABLE logins (origin_url VARCHAR NOT NULL, "
                    "action_url VARCHAR, username_element VARCHAR, "
                    "username_value VARCHAR, password_element VARCHAR, "
                    "password_value BLOB, signon_realm VARCHAR NOT NULL, "
                    "date_created INTEGER NOT NULL, "
                    "blacklisted_by_user INTEGER NOT NULL, "
                    "scheme INTEGER NOT NULL)")) {
      return false;
    }
    sql::Statement insert(db.GetUniqueStatement(
        "INSERT INTO logins VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, 0)"));
    insert.BindString(0, "https://login.test/");
    insert.BindString(1, "https://login.test/session");
    insert.BindString(2, "email");
    insert.BindString(3, "user");
    insert.BindString(4, "pass");
    insert.BindBlob(5, encrypt.Run("secret"));
    insert.BindString(6, "https://login.test/");
    insert.BindInt64(7, ChromiumNow());
    if (!insert.Run()) {
      return false;
    }
  }
  return true;
}

bool WriteFirefoxPlaces(const base::FilePath& path) {
  sql::Database db(sql::test::kTestTag);
  if (!db.Open(path) ||
      !db.Execute("CREATE TABLE moz_places (id INTEGER PRIMARY KEY, "
                  "url LONGVARCHAR, title LONGVARCHAR)") ||
      !db.Execute("CREATE TABLE moz_bookmarks (id INTEGER PRIMARY KEY, "
                  "type INTEGER, fk INTEGER DEFAULT NULL, parent INTEGER, "
                  "position INTEGER, title LONGVARCHAR, guid TEXT)")) {
    return false;
  }
  // Places, then the tree: the root (1), its folders, and the entries.
  const std::string statements[] = {
      "INSERT INTO moz_places VALUES (1, 'https://toolbar.test/', 'Page')",
      "INSERT INTO moz_places VALUES (2, 'https://news.test/daily', 'Daily')",
      "INSERT INTO moz_places VALUES (3, 'https://unfiled.test/', 'Unfiled')",
      "INSERT INTO moz_places VALUES (4, 'place:sort=8&maxResults=10', '')",
      "INSERT INTO moz_bookmarks VALUES "
      "(1, 2, NULL, 0, 0, '', 'root________')",
      "INSERT INTO moz_bookmarks VALUES "
      "(2, 2, NULL, 1, 0, 'menu', 'menu________')",
      "INSERT INTO moz_bookmarks VALUES "
      "(3, 2, NULL, 1, 1, 'toolbar', 'toolbar_____')",
      "INSERT INTO moz_bookmarks VALUES "
      "(5, 2, NULL, 1, 3, 'unfiled', 'unfiled_____')",
      "INSERT INTO moz_bookmarks VALUES "
      "(6, 2, NULL, 1, 4, 'mobile', 'mobile______')",
      "INSERT INTO moz_bookmarks VALUES "
      "(10, 1, 1, 3, 0, 'Toolbar link', 'toolbarlink1')",
      "INSERT INTO moz_bookmarks VALUES "
      "(11, 2, NULL, 3, 1, 'News', 'newsfolder01')",
      "INSERT INTO moz_bookmarks VALUES (12, 1, 2, 11, 0, NULL, 'dailylink001')",
      "INSERT INTO moz_bookmarks VALUES (13, 3, NULL, 3, 2, '', 'separator001')",
      "INSERT INTO moz_bookmarks VALUES "
      "(14, 1, 4, 2, 0, 'Most Visited', 'mostvisited1')",
      "INSERT INTO moz_bookmarks VALUES "
      "(15, 1, 3, 5, 0, 'Unfiled', 'unfiledlink1')",
  };
  for (const std::string& statement : statements) {
    if (!db.Execute(statement)) {
      return false;
    }
  }
  return true;
}

std::string MakeFirefoxSessionJson() {
  return R"json({
  "version": ["sessionrestore", 1],
  "windows": [{
    "tabs": [
      {"entries": [{"url": "https://pinned.test/", "title": "Pinned"}],
       "index": 1, "pinned": true, "lastAccessed": 1700000000000},
      {"entries": [{"url": "https://first.test/", "title": "First"},
                   {"url": "https://first.test/next", "title": "Next"}],
       "index": 2, "lastAccessed": 1700000001000},
      {"entries": [{"url": "https://read.test/a", "title": "A"}],
       "index": 1, "groupId": "g1"},
      {"entries": [{"url": "https://read.test/b", "title": "B"}],
       "index": 1, "groupId": "g1"},
      {"entries": [{"url": "about:newtab", "title": "New Tab"}], "index": 1}
    ],
    "groups": [{"id": "g1", "name": "Reading", "color": "blue",
                "collapsed": false}]
  }],
  "_closedWindows": []
})json";
}

std::string MakeZenSessionsJson() {
  return R"json({
  "spaces": [
    {"uuid": "{play}", "name": "Play", "icon": "", "position": 2000},
    {"uuid": "{work}", "name": "Work", "icon": "💼",
     "position": 1000,
     "theme": {"type": "gradient", "gradientColors": [
       {"c": [59, 130, 246], "isCustom": false, "type": "explicit-lightness"},
       {"c": [139, 92, 246], "isCustom": false, "type": "explicit-lightness"}
     ], "opacity": 0.5}}
  ],
  "folders": [
    {"id": "f-docs", "name": "Docs", "pinned": true, "parentId": null,
     "workspaceId": "{work}", "splitViewGroup": false},
    {"id": "f-specs", "name": "Specs", "pinned": true, "parentId": "f-docs",
     "workspaceId": "{work}", "splitViewGroup": false}
  ],
  "groups": [],
  "tabs": [
    {"entries": [{"url": "https://mail.test/", "title": "Mail"}],
     "index": 1, "pinned": true, "zenEssential": true,
     "zenWorkspace": "{work}", "zenSyncId": "t-mail"},
    {"entries": [{"url": "https://docs.test/guide", "title": "Guide"}],
     "index": 1, "pinned": true, "groupId": "f-docs",
     "zenWorkspace": "{work}", "zenSyncId": "t-guide"},
    {"entries": [{"url": "https://docs.test/spec", "title": "Spec"}],
     "index": 1, "pinned": true, "groupId": "f-specs",
     "zenWorkspace": "{work}", "zenSyncId": "t-spec"},
    {"entries": [{"url": "https://board.test/item/7", "title": "Item 7"}],
     "index": 1, "pinned": true, "zenWorkspace": "{work}",
     "zenSyncId": "t-board", "zenStaticLabel": "Board",
     "_zenPinnedInitialState": {"entry": {"url": "https://board.test/",
                                          "title": "Board home"}}},
    {"entries": [{"url": "https://work.test/open", "title": "Open"}],
     "index": 1, "zenWorkspace": "{work}", "zenSyncId": "t-open",
     "lastAccessed": 1700000000000},
    {"entries": [{"url": "https://play.test/", "title": "Game"}],
     "index": 1, "zenWorkspace": "{play}", "zenSyncId": "t-game"},
    {"entries": [{"url": "about:blank"}], "index": 1, "zenIsEmpty": true,
     "zenWorkspace": "{play}"},
    {"entries": [{"url": "https://glance.test/"}], "index": 1,
     "zenIsGlance": true, "zenWorkspace": "{play}"}
  ]
})json";
}

bool WriteCrestSession(
    const base::FilePath& path,
    std::string_view core,
    const std::vector<std::pair<std::string, std::string>>& history) {
  if (!base::CreateDirectory(path.DirName())) {
    return false;
  }
  sql::Database db(sql::DatabaseOptions(), "Test");
  if (!db.Open(path) ||
      !db.Execute("CREATE TABLE checkpoint (part TEXT PRIMARY KEY, "
                  "data BLOB NOT NULL)")) {
    return false;
  }
  std::vector<std::pair<std::string, std::string>> rows = {
      {"core", std::string(core)}, {"journal", "{}"}};
  for (const auto& [space, json] : history) {
    rows.emplace_back(base::StrCat({"history.", space}), json);
  }
  sql::Statement insert(
      db.GetUniqueStatement("INSERT INTO checkpoint VALUES (?, ?)"));
  for (const auto& [part, data] : rows) {
    insert.Reset(true);
    insert.BindString(0, part);
    insert.BindBlob(1, base::as_byte_span(data));
    if (!insert.Run()) {
      return false;
    }
  }
  return true;
}

}  // namespace compass::browser_import::test
