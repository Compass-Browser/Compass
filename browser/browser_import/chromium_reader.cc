// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/chromium_reader.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <utility>

#include "base/containers/span_reader.h"
#include "base/files/file_enumerator.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/numerics/byte_conversions.h"
#include "base/pickle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "compass/browser/sidebar/sidebar_serializer.h"
#include "components/sessions/core/session_command.h"
#include "components/sessions/core/session_service_commands.h"
#include "components/sessions/core/session_types.h"
#include "sql/database.h"
#include "sql/statement.h"

#if BUILDFLAG(IS_POSIX)
#include <errno.h>
#include <signal.h>
#endif

namespace compass::browser_import {

namespace {

// The session file header: "SNSS", then the version. Compass reads the
// cleartext version (3); 5 is encrypted with the browser's own key.
constexpr uint32_t kSessionSignature = 0x53534E53;
constexpr uint32_t kSessionVersion = 3;
// The commands RestoreSessionFromCommands() knows. It stops at the first
// one it doesn't, so others (newer Chromium's, Vivaldi's) are left out.
constexpr uint8_t kLastKnownCommand = 37;
constexpr uint8_t kRetiredCommands[] = {1, 3, 4, 22, 26};

// A Chromium time: microseconds since 1601 (Windows' epoch).
base::Time FromChromiumTime(int64_t micros) {
  if (micros <= 0) {
    return base::Time();
  }
  return base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(micros));
}

base::Time FromChromiumTimeString(const std::string* micros) {
  int64_t value = 0;
  if (!micros || !base::StringToInt64(*micros, &value)) {
    return base::Time();
  }
  return FromChromiumTime(value);
}

void AddBookmarks(const base::ListValue& nodes,
                  std::string_view id_prefix,
                  std::vector<arc::Item>& out) {
  for (const base::Value& value : nodes) {
    const base::DictValue* node = value.GetIfDict();
    if (!node) {
      continue;
    }
    const std::string* type = node->FindString("type");
    const std::string* guid = node->FindString("guid");
    const std::string* name = node->FindString("name");
    const std::string id =
        base::StrCat({id_prefix, "bookmark:", guid ? *guid : ""});
    const std::u16string title = name ? base::UTF8ToUTF16(*name) : u"";
    if (type && *type == "folder") {
      std::vector<arc::Item> children;
      if (const base::ListValue* list = node->FindList("children")) {
        AddBookmarks(*list, id_prefix, children);
      }
      out.push_back(MakeFolder(guid ? id : std::string(),
                               title.empty() ? u"Folder" : title,
                               std::move(children)));
      continue;
    }
    const std::string* url = node->FindString("url");
    if (!url) {
      continue;
    }
    GURL gurl(*url);
    // javascript: bookmarklets and the like don't restore as pages.
    if (!IsRestorableURL(gurl) || gurl.SchemeIs("javascript")) {
      continue;
    }
    arc::Item tab =
        MakeTab(guid ? id : std::string(), gurl, title,
                FromChromiumTimeString(node->FindString("date_last_used")));
    out.push_back(std::move(tab));
  }
}

// A JSON number or string as an ID ("1700000000000", "1.5e+12").
std::string IdString(const base::Value* value) {
  if (!value) {
    return std::string();
  }
  if (value->is_string()) {
    return value->GetString();
  }
  if (value->is_int()) {
    return base::NumberToString(value->GetInt());
  }
  if (value->is_double()) {
    // Vivaldi's IDs are millisecond times: whole numbers past int's range.
    const double number = value->GetDouble();
    if (number == std::floor(number) && std::abs(number) < 9.0e15) {
      return base::NumberToString(static_cast<int64_t>(number));
    }
    return base::NumberToString(number);
  }
  return std::string();
}

// Vivaldi's tab data (a JSON object): the workspace in it, if any.
std::string WorkspaceFromExtData(std::string_view json) {
  std::optional<base::Value> value = base::JSONReader::Read(
      json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!value || !value->is_dict()) {
    return std::string();
  }
  return IdString(value->GetDict().Find("workspaceId"));
}

bool IsKnownCommand(uint8_t id) {
  return id <= kLastKnownCommand &&
         !std::ranges::contains(kRetiredCommands, id);
}

}  // namespace

SessionTab::SessionTab() = default;
SessionTab::SessionTab(const SessionTab&) = default;
SessionTab& SessionTab::operator=(const SessionTab&) = default;
SessionTab::~SessionTab() = default;

std::vector<ChromiumProfileInfo> ParseChromiumProfiles(
    const base::DictValue& local_state) {
  std::vector<ChromiumProfileInfo> profiles;
  const base::DictValue* cache =
      local_state.FindDictByDottedPath("profile.info_cache");
  if (!cache) {
    return profiles;
  }
  std::vector<std::string> order;
  if (const base::ListValue* listed =
          local_state.FindListByDottedPath("profile.profiles_order")) {
    for (const base::Value& dir : *listed) {
      if (dir.is_string() && cache->Find(dir.GetString())) {
        order.push_back(dir.GetString());
      }
    }
  }
  std::vector<std::string> rest;
  for (const auto [dir, value] : *cache) {
    if (!std::ranges::contains(order, dir)) {
      rest.push_back(dir);
    }
  }
  std::sort(rest.begin(), rest.end(),
            [](const std::string& a, const std::string& b) {
              if ((a == "Default") != (b == "Default")) {
                return a == "Default";
              }
              return a < b;
            });
  order.insert(order.end(), rest.begin(), rest.end());
  for (const std::string& dir : order) {
    const base::DictValue* info = cache->FindDict(dir);
    if (!info || info->FindBool("is_ephemeral").value_or(false)) {
      continue;
    }
    const std::string* name = info->FindString("name");
    profiles.push_back(
        {dir, name ? base::UTF8ToUTF16(*name) : base::UTF8ToUTF16(dir)});
  }
  return profiles;
}

std::vector<arc::Item> ParseChromiumBookmarks(const base::DictValue& bookmarks,
                                              std::string_view id_prefix) {
  std::vector<arc::Item> items;
  const base::DictValue* roots = bookmarks.FindDict("roots");
  if (!roots) {
    return items;
  }
  if (const base::ListValue* bar =
          roots->FindListByDottedPath("bookmark_bar.children")) {
    AddBookmarks(*bar, id_prefix, items);
  }
  struct Root {
    const char* key;
    const char16_t* name;
  };
  for (const Root root : {Root{"other", u"Other bookmarks"},
                          Root{"synced", u"Mobile bookmarks"}}) {
    const base::DictValue* dict = roots->FindDict(root.key);
    const base::ListValue* children =
        dict ? dict->FindList("children") : nullptr;
    if (!children || children->empty()) {
      continue;
    }
    std::vector<arc::Item> entries;
    AddBookmarks(*children, id_prefix, entries);
    if (entries.empty()) {
      continue;
    }
    const std::string* name = dict->FindString("name");
    const std::string* guid = dict->FindString("guid");
    items.push_back(MakeFolder(
        base::StrCat({id_prefix, "bookmark:", guid ? *guid : root.key}),
        name && !name->empty() ? base::UTF8ToUTF16(*name) : root.name,
        std::move(entries)));
  }
  return items;
}

std::optional<std::vector<SessionTab>> ParseChromiumSessionFile(
    base::span<const uint8_t> file) {
  base::SpanReader reader(file);
  uint32_t signature = 0;
  uint32_t version = 0;
  if (!reader.ReadU32NativeEndian(signature) ||
      !reader.ReadU32NativeEndian(version) ||
      signature != kSessionSignature || version != kSessionVersion) {
    return std::nullopt;
  }
  std::vector<std::unique_ptr<sessions::SessionCommand>> commands;
  // Vivaldi's tab data, from commands upstream doesn't know.
  std::map<SessionID::id_type, std::string> workspaces;
  while (reader.remaining() > 0) {
    std::optional<size_t> size = sessions::SessionCommand::GetSerializedSize(
        reader.remaining_span(), /*encrypted=*/false);
    if (!size || *size > reader.remaining()) {
      break;  // A command cut off as the browser wrote it.
    }
    std::optional<base::span<const uint8_t>> bytes = reader.Read(*size);
    std::unique_ptr<sessions::SessionCommand> command =
        sessions::SessionCommand::Deserialize(*bytes, nullptr);
    if (!command) {
      break;
    }
    if (IsKnownCommand(command->id())) {
      commands.push_back(std::move(command));
      continue;
    }
    // Vivaldi's: a tab ID and a JSON string with the workspace.
    base::PickleIterator iterator = command->ContentsAsPickle();
    int tab_id = 0;
    std::string data;
    if (iterator.ReadInt(&tab_id) && iterator.ReadString(&data) &&
        data.starts_with("{")) {
      if (std::string workspace = WorkspaceFromExtData(data);
          !workspace.empty()) {
        workspaces[tab_id] = std::move(workspace);
      }
    }
  }

  std::vector<std::unique_ptr<sessions::SessionWindow>> windows;
  SessionID active_window = SessionID::InvalidValue();
  std::string platform_session_id;
  std::set<SessionID> discarded;
  sessions::RestoreSessionFromCommands(commands, &windows, &active_window,
                                       &platform_session_id, &discarded);

  std::vector<SessionTab> tabs;
  for (const auto& window : windows) {
    if (window->type != sessions::SessionWindow::TYPE_NORMAL) {
      continue;
    }
    std::map<std::string, std::u16string> group_titles;
    for (const auto& group : window->tab_groups) {
      group_titles[group->id.ToString()] = group->visual_data.title();
    }
    for (const auto& session_tab : window->tabs) {
      if (session_tab->navigations.empty()) {
        continue;
      }
      const int index = std::clamp(
          session_tab->current_navigation_index, 0,
          static_cast<int>(session_tab->navigations.size()) - 1);
      const sessions::SerializedNavigationEntry& navigation =
          session_tab->navigations[index];
      SessionTab tab;
      tab.url = navigation.virtual_url();
      tab.title = navigation.title();
      tab.pinned = session_tab->pinned;
      tab.last_active = session_tab->last_active_time;
      if (session_tab->group) {
        tab.group = session_tab->group->ToString();
        tab.group_title = group_titles[tab.group];
      }
      if (const auto it = workspaces.find(session_tab->tab_id.id());
          it != workspaces.end()) {
        tab.workspace = it->second;
      }
      if (const auto it = session_tab->extra_data.find("vivExtData");
          tab.workspace.empty() && it != session_tab->extra_data.end()) {
        tab.workspace = WorkspaceFromExtData(it->second);
      }
      tabs.push_back(std::move(tab));
    }
  }
  return tabs;
}

std::vector<std::vector<uint8_t>> ReadChromiumSessionFiles(
    const base::FilePath& profile_dir) {
  // Newest first, by the time in the name.
  std::vector<std::pair<int64_t, base::FilePath>> files;
  base::FileEnumerator enumerator(profile_dir.AppendASCII("Sessions"),
                                  /*recursive=*/false,
                                  base::FileEnumerator::FILES,
                                  FILE_PATH_LITERAL("Session_*"));
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    const std::string name = path.BaseName().MaybeAsASCII();
    int64_t time = 0;
    if (base::StringToInt64(std::string_view(name).substr(8), &time)) {
      files.emplace_back(time, path);
    }
  }
  std::sort(files.rbegin(), files.rend());
  std::vector<std::vector<uint8_t>> contents;
  for (const auto& [time, path] : files) {
    if (std::optional<std::vector<uint8_t>> file =
            base::ReadFileToBytes(path)) {
      contents.push_back(std::move(*file));
    }
  }
  return contents;
}

std::vector<SessionTab> ParseChromiumSessionFiles(
    const std::vector<std::vector<uint8_t>>& files) {
  for (const std::vector<uint8_t>& file : files) {
    std::optional<std::vector<SessionTab>> tabs =
        ParseChromiumSessionFile(file);
    if (tabs && !tabs->empty()) {
      return std::move(*tabs);
    }
  }
  return {};
}

std::vector<SessionTab> ReadChromiumSession(const base::FilePath& profile_dir) {
  return ParseChromiumSessionFiles(ReadChromiumSessionFiles(profile_dir));
}

std::vector<arc::Item> TodayFromTabs(const std::vector<SessionTab>& tabs,
                                     std::string_view id_prefix) {
  std::vector<arc::Item> items;
  // The folder of the group being filled, while its tabs follow each other.
  std::string open_group;
  for (size_t i = 0; i < tabs.size(); ++i) {
    const SessionTab& tab = tabs[i];
    // Not the browsers' own pages (new tab pages, about:home).
    if (!IsRestorableURL(tab.url) || tab.url.SchemeIs("about")) {
      continue;
    }
    arc::Item item =
        MakeTab(base::StrCat({id_prefix, "tab:", base::NumberToString(i), ":",
                              tab.url.spec()}),
                tab.url, tab.title, tab.last_active);
    if (tab.group.empty() || tab.group_title.empty()) {
      open_group.clear();
      items.push_back(std::move(item));
      continue;
    }
    if (open_group != tab.group) {
      open_group = tab.group;
      items.push_back(MakeFolder(
          base::StrCat({id_prefix, "group:", tab.group}), tab.group_title, {}));
    }
    items.back().children.push_back(std::move(item));
  }
  return items;
}

std::vector<VivaldiWorkspace> ParseVivaldiWorkspaces(
    const base::DictValue& preferences) {
  std::vector<VivaldiWorkspace> workspaces;
  const base::ListValue* list =
      preferences.FindListByDottedPath("vivaldi.workspaces.list");
  if (!list) {
    return workspaces;
  }
  for (const base::Value& value : *list) {
    const base::DictValue* dict = value.GetIfDict();
    if (!dict) {
      continue;
    }
    VivaldiWorkspace workspace;
    workspace.id = IdString(dict->Find("id"));
    if (workspace.id.empty()) {
      continue;
    }
    if (const std::string* name = dict->FindString("name")) {
      workspace.name = base::UTF8ToUTF16(*name);
    }
    if (const std::string* emoji = dict->FindString("emoji")) {
      workspace.emoji = *emoji;
    }
    workspaces.push_back(std::move(workspace));
  }
  return workspaces;
}

ImportedSidebar ChromiumSidebar(const SourceProfile& profile,
                                const base::DictValue* bookmarks,
                                const base::DictValue* preferences,
                                const std::vector<SessionTab>& tabs,
                                bool import_bookmarks,
                                bool import_tabs) {
  ImportedSidebar sidebar;
  const std::string prefix = base::StrCat({profile.id, ":"});
  arc::Space main;
  main.id = base::StrCat({prefix, "space"});
  main.name = SpaceNameFor(profile);
  if (import_bookmarks && bookmarks) {
    main.pinned = ParseChromiumBookmarks(*bookmarks, prefix);
  }
  if (!import_tabs) {
    sidebar.spaces.push_back(std::move(main));
    return sidebar;
  }

  // Vivaldi: tabs of a workspace go to its space.
  std::vector<VivaldiWorkspace> workspaces;
  if (profile.browser == Browser::kVivaldi && preferences) {
    workspaces = ParseVivaldiWorkspaces(*preferences);
  }
  std::vector<SessionTab> loose;
  std::vector<SessionTab> pinned;
  std::map<std::string, std::vector<SessionTab>> by_workspace;
  for (const SessionTab& tab : tabs) {
    if (profile.browser == Browser::kDia && tab.pinned) {
      pinned.push_back(tab);
      continue;
    }
    const bool known = std::ranges::any_of(
        workspaces, [&](const VivaldiWorkspace& workspace) {
          return workspace.id == tab.workspace;
        });
    (known ? by_workspace[tab.workspace] : loose).push_back(tab);
  }
  std::vector<arc::Item> pins =
      TodayFromTabs(pinned, base::StrCat({prefix, "pinned:"}));
  main.pinned.insert(main.pinned.end(), std::make_move_iterator(pins.begin()),
                     std::make_move_iterator(pins.end()));
  main.today = TodayFromTabs(loose, base::StrCat({prefix, "today:"}));
  sidebar.spaces.push_back(std::move(main));
  for (const VivaldiWorkspace& workspace : workspaces) {
    arc::Space space;
    space.id = base::StrCat({prefix, "workspace:", workspace.id});
    space.name = workspace.name.empty() ? u"Workspace" : workspace.name;
    if (std::optional<SpaceIcon> icon = SpaceIcon::Emoji(workspace.emoji)) {
      space.icon = *icon;
    }
    space.today = TodayFromTabs(
        by_workspace[workspace.id],
        base::StrCat({prefix, "workspace:", workspace.id, ":"}));
    sidebar.spaces.push_back(std::move(space));
  }
  return sidebar;
}

std::vector<HistoryEntry> ReadChromiumHistory(const base::FilePath& history,
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
  sql::Statement statement(db.GetUniqueStatement(
      "SELECT url, title, visit_count, typed_count, last_visit_time "
      "FROM urls WHERE hidden = 0 ORDER BY last_visit_time DESC LIMIT ?"));
  statement.BindInt64(0, static_cast<int64_t>(max));
  while (statement.Step()) {
    HistoryEntry entry;
    entry.url = GURL(statement.ColumnStringView(0));
    if (!entry.url.is_valid() || !entry.url.SchemeIsHTTPOrHTTPS()) {
      continue;
    }
    entry.title = statement.ColumnString16(1);
    entry.visit_count = statement.ColumnInt(2);
    entry.typed_count = statement.ColumnInt(3);
    entry.last_visit = FromChromiumTime(statement.ColumnInt64(4));
    entries.push_back(std::move(entry));
  }
  return entries;
}

std::vector<LoginEntry> ReadChromiumLogins(const base::FilePath& login_data,
                                           const PasswordDecryptor& decrypt,
                                           int* undecryptable) {
  std::vector<LoginEntry> logins;
  *undecryptable = 0;
  base::ScopedTempDir temp;
  if (!temp.CreateUniqueTempDir()) {
    return logins;
  }
  const base::FilePath copy = CopyDatabase(login_data, temp.GetPath());
  if (copy.empty()) {
    return logins;
  }
  sql::Database db(sql::DatabaseOptions(), "FirefoxImporter");
  if (!db.Open(copy)) {
    return logins;
  }
  sql::Statement statement(db.GetUniqueStatement(
      "SELECT origin_url, action_url, username_element, username_value, "
      "password_element, password_value, signon_realm, date_created, "
      "blacklisted_by_user, scheme FROM logins"));
  while (statement.Step()) {
    LoginEntry entry;
    entry.url = GURL(statement.ColumnStringView(0));
    entry.action = GURL(statement.ColumnStringView(1));
    entry.username_element = statement.ColumnString16(2);
    entry.username = statement.ColumnString16(3);
    entry.password_element = statement.ColumnString16(4);
    entry.signon_realm = statement.ColumnString(6);
    entry.created = FromChromiumTime(statement.ColumnInt64(7));
    entry.blocked = statement.ColumnBool(8);
    entry.scheme = statement.ColumnInt(9);
    if (!entry.url.is_valid() || entry.signon_realm.empty()) {
      continue;
    }
    const std::string_view encrypted =
        base::as_string_view(statement.ColumnBlob(5));
    if (!encrypted.empty()) {
      std::optional<std::string> password = decrypt.Run(encrypted);
      if (!password) {
        ++*undecryptable;
        continue;
      }
      entry.password = base::UTF8ToUTF16(*password);
    }
    logins.push_back(std::move(entry));
  }
  return logins;
}

bool IsChromiumRunning(const base::FilePath& user_data) {
#if BUILDFLAG(IS_WIN)
  // Held open (write, no sharing) and deleted on close by the browser.
  const base::FilePath lock = user_data.AppendASCII("lockfile");
  if (!base::PathExists(lock)) {
    return false;
  }
  base::File file(lock, base::File::FLAG_OPEN | base::File::FLAG_WRITE);
  return !file.IsValid();
#elif BUILDFLAG(IS_POSIX)
  // A symbolic link to "<host>-<pid>".
  base::FilePath target;
  if (!base::ReadSymbolicLink(user_data.AppendASCII("SingletonLock"),
                              &target)) {
    return false;
  }
  const std::string value = target.value();
  const size_t dash = value.rfind('-');
  int pid = 0;
  if (dash == std::string::npos ||
      !base::StringToInt(std::string_view(value).substr(dash + 1), &pid) ||
      pid <= 0) {
    return false;
  }
  return kill(pid, 0) == 0 || errno == EPERM;
#else
  return false;
#endif
}

}  // namespace compass::browser_import
