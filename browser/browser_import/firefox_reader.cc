// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/firefox_reader.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <utility>

#include "base/containers/span_reader.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "compass/browser/sidebar/sidebar_serializer.h"
#include "compass/browser/sidebar/space_model.h"
#include "sql/database.h"
#include "sql/statement.h"

#if BUILDFLAG(IS_POSIX)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace compass::browser_import {

namespace {

constexpr std::string_view kMozLz4Magic("mozLz40\0", 8);
// mozLz4 files hold sessions of some megabytes; refuse absurd sizes.
constexpr uint32_t kMaxMozLz4Size = 512 * 1024 * 1024;

// A Firefox time: milliseconds since 1970.
base::Time FromMilliseconds(std::optional<double> ms) {
  if (!ms || *ms <= 0) {
    return base::Time();
  }
  return base::Time::FromMillisecondsSinceUnixEpoch(*ms);
}

// A session store tab's current page: its entry at "index" (1-based).
const base::DictValue* CurrentEntry(const base::DictValue& tab) {
  const base::ListValue* entries = tab.FindList("entries");
  if (!entries || entries->empty()) {
    return nullptr;
  }
  int index = tab.FindInt("index").value_or(static_cast<int>(entries->size()));
  index = std::clamp(index, 1, static_cast<int>(entries->size()));
  return (*entries)[index - 1].GetIfDict();
}

std::string StringOr(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

SessionTab ToSessionTab(const base::DictValue& tab,
                        const std::map<std::string, std::u16string>& groups) {
  SessionTab session_tab;
  if (const base::DictValue* entry = CurrentEntry(tab)) {
    session_tab.url = GURL(StringOr(*entry, "url"));
    session_tab.title = base::UTF8ToUTF16(StringOr(*entry, "title"));
  }
  session_tab.pinned = tab.FindBool("pinned").value_or(false);
  session_tab.last_active = FromMilliseconds(tab.FindDouble("lastAccessed"));
  session_tab.group = StringOr(tab, "groupId");
  if (const auto it = groups.find(session_tab.group); it != groups.end()) {
    session_tab.group_title = it->second;
  }
  return session_tab;
}

std::map<std::string, std::u16string> GroupNames(const base::DictValue& window) {
  std::map<std::string, std::u16string> names;
  if (const base::ListValue* groups = window.FindList("groups")) {
    for (const base::Value& value : *groups) {
      if (const base::DictValue* group = value.GetIfDict()) {
        names[StringOr(*group, "id")] =
            base::UTF8ToUTF16(StringOr(*group, "name"));
      }
    }
  }
  return names;
}

// Zen ------------------------------------------------------------------------

// A Zen space's theme: its gradient's colors, the first as the space's.
void ApplyZenTheme(const base::DictValue& space, arc::Space& out) {
  const base::ListValue* colors =
      space.FindListByDottedPath("theme.gradientColors");
  if (!colors) {
    return;
  }
  std::vector<SpaceColor> found;
  for (const base::Value& value : *colors) {
    const base::DictValue* color = value.GetIfDict();
    const base::ListValue* rgb = color ? color->FindList("c") : nullptr;
    if (!rgb || rgb->size() != 3) {
      continue;
    }
    std::array<uint8_t, 3> channels;
    bool ok = true;
    for (size_t i = 0; i < 3; ++i) {
      const std::optional<double> channel = (*rgb)[i].GetIfDouble();
      if (!channel) {
        ok = false;
        break;
      }
      channels[i] =
          static_cast<uint8_t>(std::clamp(*channel, 0.0, 255.0) + 0.5);
    }
    if (ok) {
      found.push_back(
          SpaceColor::FromRGB(channels[0], channels[1], channels[2]));
    }
    if (found.size() == kMaxSpaceGradientColors) {
      break;
    }
  }
  if (found.empty()) {
    return;
  }
  out.color = found[0];
  out.gradient.assign(found.begin() + 1, found.end());
}

// Builds a Zen space's trees: Pinned and Today roots, and folders, filled
// in the order tabs come.
class ZenTreeBuilder {
 public:
  ZenTreeBuilder(std::string id_prefix,
                 const std::vector<const base::DictValue*>& folders)
      : id_prefix_(std::move(id_prefix)) {
    for (const base::DictValue* folder : folders) {
      const std::string id = StringOr(*folder, "id");
      if (!id.empty() && !folders_.contains(id)) {
        folders_[id] = folder;
      }
    }
  }

  // Adds |tab| at the end of |folder_id|'s folder (empty: the root) of
  // |space_id|'s Pinned or Today.
  void Add(const std::string& space_id,
           bool pinned,
           const std::string& folder_id,
           arc::Item tab) {
    const std::string slot = folder_id.empty() || !folders_.contains(folder_id)
                                 ? RootSlot(space_id, pinned)
                                 : Attach(space_id, pinned, folder_id);
    slots_[slot].push_back({std::move(tab), std::string()});
  }

  std::vector<arc::Item> Build(const std::string& space_id, bool pinned) {
    return Materialize(RootSlot(space_id, pinned));
  }

 private:
  struct Entry {
    arc::Item tab;
    // Set for a folder: its ID.
    std::string folder;
  };

  static std::string RootSlot(const std::string& space_id, bool pinned) {
    return base::StrCat({pinned ? "pinned:" : "today:", space_id});
  }

  // The slot of |folder_id|, placed in its parent folder (or the root) the
  // first time.
  std::string Attach(const std::string& space_id,
                     bool pinned,
                     const std::string& folder_id) {
    const std::string slot = base::StrCat({"folder:", folder_id});
    if (attached_.contains(folder_id)) {
      return slot;
    }
    attached_.insert(folder_id);
    const base::DictValue& folder = *folders_[folder_id];
    const std::string parent = StringOr(folder, "parentId");
    const std::string parent_slot =
        !parent.empty() && folders_.contains(parent) &&
                !attached_.contains(parent)
            ? Attach(space_id, pinned, parent)
            : (!parent.empty() && folders_.contains(parent)
                   ? base::StrCat({"folder:", parent})
                   : RootSlot(space_id, pinned));
    slots_[parent_slot].push_back({arc::Item(), folder_id});
    return slot;
  }

  std::vector<arc::Item> Materialize(const std::string& slot) {
    std::vector<arc::Item> items;
    for (Entry& entry : slots_[slot]) {
      if (entry.folder.empty()) {
        items.push_back(std::move(entry.tab));
        continue;
      }
      const base::DictValue& folder = *folders_[entry.folder];
      std::vector<arc::Item> children =
          Materialize(base::StrCat({"folder:", entry.folder}));
      if (children.empty()) {
        continue;
      }
      std::u16string name = base::UTF8ToUTF16(StringOr(folder, "name"));
      arc::Item item =
          MakeFolder(base::StrCat({id_prefix_, "folder:", entry.folder}),
                     name.empty() ? u"Folder" : std::move(name),
                     std::move(children));
      if (folder.FindBool("splitViewGroup").value_or(false)) {
        item.kind = arc::Item::Kind::kSplit;
      }
      items.push_back(std::move(item));
    }
    return items;
  }

  const std::string id_prefix_;
  std::map<std::string, const base::DictValue*> folders_;
  std::set<std::string> attached_;
  std::map<std::string, std::vector<Entry>> slots_;
};

}  // namespace

std::vector<FirefoxProfileInfo> ParseProfilesIni(std::string_view ini,
                                                 const base::FilePath& root) {
  // Sections in order, each a key/value map.
  std::vector<std::pair<std::string, std::map<std::string, std::string>>>
      sections;
  for (std::string_view line : base::SplitStringPiece(
           ini, "\r\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (line.starts_with(";") || line.starts_with("#")) {
      continue;
    }
    if (line.starts_with("[") && line.ends_with("]")) {
      sections.emplace_back(std::string(line.substr(1, line.size() - 2)),
                            std::map<std::string, std::string>());
      continue;
    }
    const size_t equals = line.find('=');
    if (sections.empty() || equals == std::string_view::npos) {
      continue;
    }
    sections.back().second[std::string(base::TrimWhitespaceASCII(
        line.substr(0, equals), base::TRIM_ALL))] =
        std::string(
            base::TrimWhitespaceASCII(line.substr(equals + 1), base::TRIM_ALL));
  }

  std::string install_default;
  for (const auto& [name, values] : sections) {
    if (name.starts_with("Install")) {
      if (const auto it = values.find("Default"); it != values.end()) {
        install_default = it->second;
        break;
      }
    }
  }

  std::vector<FirefoxProfileInfo> profiles;
  bool have_default = false;
  for (const auto& [name, values] : sections) {
    if (!name.starts_with("Profile")) {
      continue;
    }
    const auto path = values.find("Path");
    if (path == values.end() || path->second.empty()) {
      continue;
    }
    const auto relative = values.find("IsRelative");
    const bool is_relative = relative == values.end() || relative->second == "1";
    base::FilePath dir = base::FilePath::FromUTF8Unsafe(path->second);
    if (is_relative) {
      dir = root.Append(dir);
    }
    FirefoxProfileInfo info;
    info.path = dir.NormalizePathSeparators();
    const auto profile_name = values.find("Name");
    info.name = base::UTF8ToUTF16(profile_name != values.end()
                                      ? profile_name->second
                                      : path->second);
    info.is_default = install_default.empty()
                          ? values.contains("Default") &&
                                values.at("Default") == "1"
                          : path->second == install_default;
    if (info.is_default && have_default) {
      info.is_default = false;
    }
    have_default |= info.is_default;
    profiles.push_back(std::move(info));
  }
  std::stable_partition(profiles.begin(), profiles.end(),
                        [](const FirefoxProfileInfo& info) {
                          return info.is_default;
                        });
  return profiles;
}

std::optional<std::string> DecompressMozLz4(base::span<const uint8_t> file) {
  base::SpanReader reader(file);
  std::optional<base::span<const uint8_t>> magic =
      reader.Read(kMozLz4Magic.size());
  uint32_t size = 0;
  if (!magic || base::as_string_view(*magic) != kMozLz4Magic ||
      !reader.ReadU32LittleEndian(size) || size > kMaxMozLz4Size) {
    return std::nullopt;
  }
  std::string out;
  out.reserve(size);
  // LZ4 block format: sequences of a token, literals, an offset and a match.
  auto read_length = [&](size_t length) -> std::optional<size_t> {
    if (length != 15) {
      return length;
    }
    uint8_t more = 255;
    while (more == 255) {
      if (!reader.ReadU8NativeEndian(more)) {
        return std::nullopt;
      }
      length += more;
      if (length > size) {
        return std::nullopt;
      }
    }
    return length;
  };
  while (reader.remaining() > 0) {
    uint8_t token = 0;
    reader.ReadU8NativeEndian(token);
    std::optional<size_t> literals = read_length(token >> 4);
    if (!literals) {
      return std::nullopt;
    }
    std::optional<base::span<const uint8_t>> bytes = reader.Read(*literals);
    if (!bytes || out.size() + bytes->size() > size) {
      return std::nullopt;
    }
    out.append(base::as_string_view(*bytes));
    if (reader.remaining() == 0) {
      break;  // The last sequence has literals only.
    }
    uint16_t offset = 0;
    if (!reader.ReadU16LittleEndian(offset) || offset == 0 ||
        offset > out.size()) {
      return std::nullopt;
    }
    std::optional<size_t> match = read_length(token & 0x0f);
    if (!match) {
      return std::nullopt;
    }
    const size_t length = *match + 4;
    if (out.size() + length > size) {
      return std::nullopt;
    }
    // Byte by byte: a match may overlap what it writes.
    const size_t from = out.size() - offset;
    for (size_t i = 0; i < length; ++i) {
      const char byte = out[from + i];
      out.push_back(byte);
    }
  }
  if (out.size() != size) {
    return std::nullopt;
  }
  return out;
}

std::vector<uint8_t> CompressMozLz4ForTesting(std::string_view text) {
  std::vector<uint8_t> file(kMozLz4Magic.begin(), kMozLz4Magic.end());
  const uint32_t size = static_cast<uint32_t>(text.size());
  for (int i = 0; i < 4; ++i) {
    file.push_back(static_cast<uint8_t>(size >> (8 * i)));
  }
  // One sequence of literals.
  if (text.size() < 15) {
    file.push_back(static_cast<uint8_t>(text.size() << 4));
  } else {
    file.push_back(0xf0);
    size_t rest = text.size() - 15;
    while (rest >= 255) {
      file.push_back(255);
      rest -= 255;
    }
    file.push_back(static_cast<uint8_t>(rest));
  }
  file.insert(file.end(), text.begin(), text.end());
  return file;
}

std::optional<base::DictValue> ReadMozLz4Json(const base::FilePath& path) {
  std::optional<std::vector<uint8_t>> file = base::ReadFileToBytes(path);
  if (!file) {
    return std::nullopt;
  }
  std::optional<std::string> json = DecompressMozLz4(*file);
  if (!json) {
    return std::nullopt;
  }
  std::optional<base::Value> value =
      base::JSONReader::Read(*json, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    return std::nullopt;
  }
  return std::move(*value).TakeDict();
}

std::vector<arc::Item> ReadFirefoxBookmarks(const base::FilePath& places,
                                            std::string_view id_prefix) {
  std::vector<arc::Item> items;
  base::ScopedTempDir temp;
  if (!temp.CreateUniqueTempDir()) {
    return items;
  }
  const base::FilePath copy = CopyDatabase(places, temp.GetPath());
  if (copy.empty()) {
    return items;
  }
  sql::Database db(sql::DatabaseOptions(), "FirefoxImporter");
  if (!db.Open(copy)) {
    return items;
  }
  struct Row {
    int64_t id;
    int type;
    std::string title;
    std::string guid;
    std::string url;
  };
  std::map<int64_t, std::vector<Row>> children;
  std::map<std::string, int64_t> roots;
  sql::Statement statement(db.GetUniqueStatement(
      "SELECT b.id, b.type, b.parent, b.title, b.guid, p.url, p.title "
      "FROM moz_bookmarks b LEFT JOIN moz_places p ON b.fk = p.id "
      "ORDER BY b.parent, b.position"));
  while (statement.Step()) {
    Row row;
    row.id = statement.ColumnInt64(0);
    row.type = statement.ColumnInt(1);
    row.title = statement.ColumnString(3);
    if (row.title.empty()) {
      row.title = statement.ColumnString(6);
    }
    row.guid = statement.ColumnString(4);
    row.url = statement.ColumnString(5);
    roots[row.guid] = row.id;
    children[statement.ColumnInt64(2)].push_back(std::move(row));
  }

  // Bookmarks are type 1, folders 2, separators 3.
  auto build = [&](auto& self, int64_t parent) -> std::vector<arc::Item> {
    std::vector<arc::Item> out;
    for (const Row& row : children[parent]) {
      const std::string id = base::StrCat({id_prefix, "bookmark:", row.guid});
      if (row.type == 2) {
        std::u16string title = base::UTF8ToUTF16(row.title);
        out.push_back(MakeFolder(id, title.empty() ? u"Folder" : title,
                                 self(self, row.id)));
      } else if (row.type == 1) {
        GURL url(row.url);
        if (!IsRestorableURL(url) || url.SchemeIs("place") ||
            url.SchemeIs("javascript")) {
          continue;
        }
        out.push_back(MakeTab(id, url, base::UTF8ToUTF16(row.title)));
      }
    }
    return out;
  };

  if (const auto toolbar = roots.find("toolbar_____"); toolbar != roots.end()) {
    items = build(build, toolbar->second);
  }
  struct Root {
    const char* guid;
    const char16_t* name;
  };
  for (const Root root : {Root{"menu________", u"Bookmarks Menu"},
                          Root{"unfiled_____", u"Other Bookmarks"},
                          Root{"mobile______", u"Mobile Bookmarks"}}) {
    const auto it = roots.find(root.guid);
    if (it == roots.end()) {
      continue;
    }
    std::vector<arc::Item> entries = build(build, it->second);
    if (!entries.empty()) {
      items.push_back(MakeFolder(base::StrCat({id_prefix, "bookmark:",
                                               root.guid}),
                                 root.name, std::move(entries)));
    }
  }
  return items;
}

std::vector<SessionTab> ParseFirefoxSessionTabs(
    const base::DictValue& session) {
  std::vector<SessionTab> tabs;
  const base::ListValue* windows = session.FindList("windows");
  if (!windows) {
    return tabs;
  }
  for (const base::Value& window_value : *windows) {
    const base::DictValue* window = window_value.GetIfDict();
    const base::ListValue* window_tabs =
        window ? window->FindList("tabs") : nullptr;
    if (!window_tabs) {
      continue;
    }
    const std::map<std::string, std::u16string> groups = GroupNames(*window);
    for (const base::Value& tab : *window_tabs) {
      if (const base::DictValue* dict = tab.GetIfDict()) {
        tabs.push_back(ToSessionTab(*dict, groups));
      }
    }
  }
  return tabs;
}

std::optional<base::DictValue> ReadFirefoxSession(
    const base::FilePath& profile_dir) {
  const base::FilePath candidates[] = {
      profile_dir.AppendASCII("sessionstore.jsonlz4"),
      profile_dir.AppendASCII("sessionstore-backups")
          .AppendASCII("recovery.jsonlz4"),
      profile_dir.AppendASCII("sessionstore-backups")
          .AppendASCII("previous.jsonlz4"),
  };
  // The newest that reads.
  std::vector<std::pair<base::Time, base::FilePath>> files;
  for (const base::FilePath& path : candidates) {
    base::File::Info info;
    if (base::GetFileInfo(path, &info)) {
      files.emplace_back(info.last_modified, path);
    }
  }
  std::sort(files.rbegin(), files.rend());
  for (const auto& [time, path] : files) {
    if (std::optional<base::DictValue> session = ReadMozLz4Json(path)) {
      return session;
    }
  }
  return std::nullopt;
}

std::optional<base::DictValue> ReadZenSidebarState(
    const base::FilePath& profile_dir) {
  for (const base::FilePath& path :
       {profile_dir.AppendASCII("zen-sessions.jsonlz4"),
        profile_dir.AppendASCII("zen-sessions-backup")
            .AppendASCII("clean.jsonlz4")}) {
    std::optional<base::DictValue> state = ReadMozLz4Json(path);
    const base::ListValue* spaces = state ? state->FindList("spaces") : nullptr;
    if (spaces && !spaces->empty()) {
      return state;
    }
  }
  std::optional<base::DictValue> session = ReadFirefoxSession(profile_dir);
  base::ListValue* windows = session ? session->FindList("windows") : nullptr;
  if (!windows || windows->empty() || !(*windows)[0].is_dict()) {
    return std::nullopt;
  }
  // The first window carries the spaces; every window's tabs count.
  base::DictValue state = std::move((*windows)[0].GetDict());
  base::ListValue tabs;
  if (base::ListValue* first = state.FindList("tabs")) {
    tabs = std::move(*first);
  }
  for (size_t i = 1; i < windows->size(); ++i) {
    base::DictValue* window = (*windows)[i].GetIfDict();
    base::ListValue* more = window ? window->FindList("tabs") : nullptr;
    if (more) {
      for (base::Value& tab : *more) {
        tabs.Append(std::move(tab));
      }
    }
  }
  state.Set("tabs", std::move(tabs));
  return state;
}

ImportedSidebar FirefoxSidebar(const SourceProfile& profile,
                               std::vector<arc::Item> bookmarks,
                               const base::DictValue* session,
                               bool import_tabs) {
  ImportedSidebar sidebar;
  const std::string prefix = base::StrCat({profile.id, ":"});
  arc::Space space;
  space.id = base::StrCat({prefix, "space"});
  space.name = SpaceNameFor(profile);
  space.pinned = std::move(bookmarks);
  if (import_tabs && session) {
    space.today = TodayFromTabs(ParseFirefoxSessionTabs(*session),
                                base::StrCat({prefix, "today:"}));
  }
  sidebar.spaces.push_back(std::move(space));
  return sidebar;
}

ImportedSidebar ZenSidebar(const SourceProfile& profile,
                           std::vector<arc::Item> bookmarks,
                           const base::DictValue* state,
                           bool import_tabs) {
  ImportedSidebar sidebar;
  const std::string prefix = base::StrCat({profile.id, ":"});

  if (import_tabs && state) {
    // Spaces, in Zen's order.
    std::vector<const base::DictValue*> spaces;
    if (const base::ListValue* list = state->FindList("spaces")) {
      for (const base::Value& value : *list) {
        if (const base::DictValue* space = value.GetIfDict();
            space && !StringOr(*space, "uuid").empty()) {
          spaces.push_back(space);
        }
      }
    }
    std::stable_sort(spaces.begin(), spaces.end(),
                     [](const base::DictValue* a, const base::DictValue* b) {
                       return a->FindDouble("position").value_or(0) <
                              b->FindDouble("position").value_or(0);
                     });
    // Folders (Zen's, pinned) and tab groups (Firefox's, in Today).
    std::vector<const base::DictValue*> folders;
    for (const char* key : {"folders", "groups"}) {
      if (const base::ListValue* list = state->FindList(key)) {
        for (const base::Value& value : *list) {
          if (const base::DictValue* folder = value.GetIfDict()) {
            folders.push_back(folder);
          }
        }
      }
    }
    std::map<std::string, std::string> folder_space;
    for (const base::DictValue* folder : folders) {
      folder_space[StringOr(*folder, "id")] =
          StringOr(*folder, "workspaceId");
    }

    ZenTreeBuilder builder(prefix, folders);
    const std::string first_space =
        spaces.empty() ? std::string() : StringOr(*spaces[0], "uuid");
    std::set<std::string> space_ids;
    for (const base::DictValue* space : spaces) {
      space_ids.insert(StringOr(*space, "uuid"));
    }
    std::set<GURL> essentials;
    size_t index = 0;
    if (const base::ListValue* tabs = state->FindList("tabs")) {
      for (const base::Value& value : *tabs) {
        const base::DictValue* tab = value.GetIfDict();
        if (!tab || tab->FindBool("zenIsEmpty").value_or(false) ||
            tab->FindBool("zenIsGlance").value_or(false)) {
          continue;
        }
        SessionTab session_tab = ToSessionTab(*tab, {});
        const bool pinned = tab->FindBool("pinned").value_or(false);
        // A pinned tab's own page, before the user browsed away from it.
        if (const base::DictValue* initial =
                tab->FindDictByDottedPath("_zenPinnedInitialState.entry");
            pinned && initial) {
          const GURL url(StringOr(*initial, "url"));
          if (url.is_valid()) {
            session_tab.url = url;
            session_tab.title = base::UTF8ToUTF16(StringOr(*initial, "title"));
          }
        }
        if (!IsRestorableURL(session_tab.url) ||
            session_tab.url.SchemeIs("about")) {
          continue;
        }
        const std::string sync_id = StringOr(*tab, "zenSyncId");
        arc::Item item = MakeTab(
            base::StrCat({prefix, "tab:",
                          sync_id.empty() ? base::NumberToString(index)
                                          : sync_id}),
            session_tab.url, session_tab.title, session_tab.last_active);
        ++index;
        if (const std::string* label = tab->FindString("zenStaticLabel")) {
          item.title = base::UTF8ToUTF16(*label);
        }
        if (tab->FindBool("zenEssential").value_or(false)) {
          if (essentials.insert(item.url).second) {
            sidebar.favorites.push_back(std::move(item));
          }
          continue;
        }
        std::string space_id = StringOr(*tab, "zenWorkspace");
        if (!space_ids.contains(space_id)) {
          const auto it = folder_space.find(session_tab.group);
          space_id = it != folder_space.end() && space_ids.contains(it->second)
                         ? it->second
                         : first_space;
        }
        if (space_id.empty()) {
          continue;
        }
        builder.Add(space_id, pinned, session_tab.group, std::move(item));
      }
    }

    for (const base::DictValue* space : spaces) {
      const std::string uuid = StringOr(*space, "uuid");
      arc::Space out;
      out.id = base::StrCat({prefix, "space:", uuid});
      std::u16string name = base::UTF8ToUTF16(StringOr(*space, "name"));
      out.name = name.empty() ? u"Space" : std::move(name);
      // An emoji; Zen's own icons are chrome:// paths.
      const std::string icon = StringOr(*space, "icon");
      if (!icon.empty() && icon.find_first_of(":/") == std::string::npos) {
        if (std::optional<SpaceIcon> emoji = SpaceIcon::Emoji(icon)) {
          out.icon = *emoji;
        }
      }
      ApplyZenTheme(*space, out);
      out.pinned = builder.Build(uuid, /*pinned=*/true);
      out.today = builder.Build(uuid, /*pinned=*/false);
      sidebar.spaces.push_back(std::move(out));
    }
  }

  if (!bookmarks.empty() || sidebar.spaces.empty()) {
    arc::Space space;
    space.id = base::StrCat({prefix, "space"});
    space.name = SpaceNameFor(profile);
    space.pinned = std::move(bookmarks);
    sidebar.spaces.push_back(std::move(space));
  }
  return sidebar;
}

base::FilePath FirefoxAppPath(const base::FilePath& profile_dir) {
  std::string ini;
  if (!base::ReadFileToString(profile_dir.AppendASCII("compatibility.ini"),
                              &ini)) {
    return base::FilePath();
  }
  for (std::string_view line : base::SplitStringPiece(
           ini, "\r\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (line.starts_with("LastPlatformDir=")) {
      return base::FilePath::FromUTF8Unsafe(
          line.substr(std::string_view("LastPlatformDir=").size()));
    }
  }
  return base::FilePath();
}

bool IsFirefoxProfileInUse(const base::FilePath& profile_dir) {
#if BUILDFLAG(IS_WIN)
  // Held open without sharing while the browser runs.
  const base::FilePath lock = profile_dir.AppendASCII("parent.lock");
  if (!base::PathExists(lock)) {
    return false;
  }
  base::File file(lock, base::File::FLAG_OPEN | base::File::FLAG_READ);
  return !file.IsValid();
#elif BUILDFLAG(IS_POSIX)
  // An fcntl lock on .parentlock while the browser runs.
  const base::FilePath lock = profile_dir.AppendASCII(".parentlock");
  const int fd = open(lock.value().c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  struct flock query = {};
  query.l_type = F_WRLCK;
  query.l_whence = SEEK_SET;
  const bool held = fcntl(fd, F_GETLK, &query) == 0 && query.l_type != F_UNLCK;
  close(fd);
  return held;
#else
  return false;
#endif
}

}  // namespace compass::browser_import
