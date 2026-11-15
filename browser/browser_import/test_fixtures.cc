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

}  // namespace compass::browser_import::test
