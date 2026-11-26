// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/yab_reader.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "compass/browser/arc_import/arc_sidebar.h"
#include "compass/browser/browser_import/app_sidebar.h"

#if BUILDFLAG(IS_MAC)
#include "compass/browser/browser_import/safari_reader.h"
#include "compass/strings/grit/compass_strings.h"
#include "ui/base/l10n/l10n_util.h"
#endif

namespace compass::browser_import {

namespace {

// Yab's first space, whose tabs are in session.json.
constexpr std::string_view kFirstSpaceId =
    "00000000-0000-0000-0000-000000000001";
// The row of the pins every space shows.
constexpr std::string_view kSharedPinsId =
    "00000000-0000-0000-0000-000000000A11";

// The profile a space signs in with (Space.profileID), upper case: its own
// (or the one it shares, signInWith) with sign-ins of its own, else the
// first space's.
std::string ProfileKeyOf(const base::DictValue& space) {
  std::string key;
  if (space.FindBool("ownSignIns").value_or(false)) {
    key = StringAt(space, "signInWith");
    if (key.empty()) {
      key = StringAt(space, "id");
    }
  }
  return base::ToUpperASCII(key.empty() ? std::string(kFirstSpaceId) : key);
}

// Bookmark folders nest at most this deep (a guard against a malformed
// file).
constexpr int kMaxDepth = 32;

void ApplyYabPaint(const base::DictValue& space, arc::Space& out) {
  const base::DictValue* paint = space.FindDict("paint");
  if (!paint) {
    return;
  }
  std::vector<SpaceColor> found;
  if (const base::ListValue* colors = paint->FindList("colors")) {
    for (const base::Value& value : *colors) {
      if (!value.is_string()) {
        continue;
      }
      if (std::optional<SpaceColor> color = HexSpaceColor(value.GetString());
          color && !color->is_grey()) {
        found.push_back(*color);
      }
      if (found.size() == kMaxSpaceGradientColors) {
        break;
      }
    }
  }
  if (found.empty()) {
    if (std::optional<SpaceColor> color =
            HexSpaceColor(StringAt(*paint, "color"));
        color && !color->is_grey()) {
      found.push_back(*color);
    }
  }
  if (found.empty()) {
    return;
  }
  out.color = found[0];
  out.gradient.assign(found.begin() + 1, found.end());
  if (std::optional<double> grain = paint->FindDouble("grain")) {
    out.grain = std::clamp(*grain, 0.0, 1.0);
  }
}

// The folders a tab is in, from the top down: folderPath's, then its own.
std::vector<FolderStep> FolderPathOf(const base::DictValue& tab) {
  std::vector<FolderStep> path;
  std::set<std::string> seen;
  if (const base::ListValue* steps = tab.FindList("folderPath")) {
    for (const base::Value& value : *steps) {
      const base::DictValue* step = value.GetIfDict();
      const std::string id = step ? StringAt(*step, "id") : std::string();
      if (!id.empty() && seen.insert(id).second) {
        path.push_back(
            FolderStep{id, base::UTF8ToUTF16(StringAt(*step, "name"))});
      }
    }
  }
  const std::string folder = StringAt(tab, "folder");
  if (!folder.empty() && seen.insert(folder).second) {
    path.push_back(
        FolderStep{folder, base::UTF8ToUTF16(StringAt(tab, "folderName"))});
  }
  return path;
}

std::optional<arc::Item> ConvertTab(const base::DictValue& tab,
                                    bool kept,
                                    const std::string& id_prefix,
                                    size_t index) {
  const std::string home = StringAt(tab, "home");
  const GURL url(kept && !home.empty() ? home : StringAt(tab, "url"));
  if (!IsImportableTabURL(url)) {
    return std::nullopt;
  }
  std::string title = kept ? StringAt(tab, "homeTitle") : std::string();
  if (title.empty()) {
    title = StringAt(tab, "title");
  }
  base::Time last_active;
  if (std::optional<double> seen = tab.FindDouble("seen")) {
    last_active = base::Time::FromSecondsSinceUnixEpoch(*seen);
  }
  std::string id = StringAt(tab, "id");
  if (id.empty()) {
    id = base::NumberToString(index);
  }
  arc::Item item = MakeTab(base::StrCat({id_prefix, "tab:", id}), url,
                           base::UTF8ToUTF16(title), last_active);
  item.title = base::UTF8ToUTF16(StringAt(tab, "name"));
  return item;
}

// One space's (or the shared pins') session: pins, kept tabs, today's.
struct YabTabs {
  std::vector<arc::Item> pins;
  std::vector<arc::Item> kept;
  std::vector<arc::Item> today;
};

YabTabs ParseSession(const base::DictValue& session,
                     const std::string& id_prefix) {
  YabTabs out;
  const base::ListValue* tabs = session.FindList("tabs");
  if (!tabs) {
    return out;
  }
  // Side by side: each tab's partner, the first of a pair leading.
  std::map<size_t, size_t> partner;
  if (const base::ListValue* pairs = session.FindList("pairs")) {
    for (const base::Value& value : *pairs) {
      const base::DictValue* pair = value.GetIfDict();
      const std::optional<int> first =
          pair ? pair->FindInt("first") : std::nullopt;
      const std::optional<int> second =
          pair ? pair->FindInt("second") : std::nullopt;
      if (!first || !second || *first < 0 || *second < 0 || *first == *second ||
          static_cast<size_t>(*first) >= tabs->size() ||
          static_cast<size_t>(*second) >= tabs->size()) {
        continue;
      }
      const size_t a = static_cast<size_t>(std::min(*first, *second));
      const size_t b = static_cast<size_t>(std::max(*first, *second));
      if (!partner.contains(a) && !partner.contains(b)) {
        partner[a] = b;
        partner[b] = a;
      }
    }
  }

  FolderTreeBuilder kept(id_prefix);
  FolderTreeBuilder today(id_prefix);
  std::set<size_t> done;
  for (size_t i = 0; i < tabs->size(); ++i) {
    const base::DictValue* tab = (*tabs)[i].GetIfDict();
    if (!tab || done.contains(i)) {
      continue;
    }
    done.insert(i);
    const bool pin = tab->FindString("pin") != nullptr;
    const bool is_kept = pin || tab->FindBool("kept").value_or(false);
    std::optional<arc::Item> item = ConvertTab(*tab, is_kept, id_prefix, i);
    if (!item) {
      continue;
    }
    // A pair in the same section stays together, as a split.
    if (const auto it = partner.find(i); it != partner.end() && !pin) {
      const base::DictValue* other = (*tabs)[it->second].GetIfDict();
      const bool other_kept = other && other->FindString("pin") == nullptr &&
                              other->FindBool("kept").value_or(false);
      if (other && other->FindString("pin") == nullptr &&
          other_kept == is_kept) {
        if (std::optional<arc::Item> second =
                ConvertTab(*other, is_kept, id_prefix, it->second)) {
          done.insert(it->second);
          std::vector<arc::Item> panes;
          panes.push_back(std::move(*item));
          panes.push_back(std::move(*second));
          item = MakeFolder(base::StrCat({id_prefix, "split:", panes[0].id}),
                            u"", std::move(panes));
          item->kind = arc::Item::Kind::kSplit;
        }
      }
    }
    if (pin) {
      out.pins.push_back(std::move(*item));
    } else {
      (is_kept ? kept : today).Add(FolderPathOf(*tab), std::move(*item));
    }
  }
  out.kept = std::move(kept).Build();
  out.today = std::move(today).Build();
  return out;
}

void AddBookmarks(const base::ListValue& list,
                  const std::string& id_prefix,
                  int depth,
                  std::vector<arc::Item>& out) {
  if (depth > kMaxDepth) {
    return;
  }
  for (const base::Value& value : list) {
    const base::DictValue* bookmark = value.GetIfDict();
    if (!bookmark) {
      continue;
    }
    const std::string id =
        base::StrCat({id_prefix, "bookmark:", StringAt(*bookmark, "id")});
    const std::u16string title =
        base::UTF8ToUTF16(StringAt(*bookmark, "title"));
    if (const std::string* url = bookmark->FindString("url")) {
      const GURL gurl(*url);
      if (IsImportableTabURL(gurl)) {
        out.push_back(MakeTab(id, gurl, title));
      }
      continue;
    }
    std::vector<arc::Item> children;
    if (const base::ListValue* list_children = bookmark->FindList("children")) {
      AddBookmarks(*list_children, id_prefix, depth + 1, children);
    }
    if (!children.empty()) {
      out.push_back(MakeFolder(
          id,
          title.empty() ? l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_FOLDER)
                        : title,
          std::move(children)));
    }
  }
}

std::optional<base::DictValue> ReadDict(const base::FilePath& path) {
  std::optional<base::Value> value = ReadJsonFile(path);
  if (!value || !value->is_dict()) {
    return std::nullopt;
  }
  return std::move(*value).TakeDict();
}

// Whether Yab shows its shared pins in every space ("pins.everySpace" in its
// defaults); nullopt if that can't be read.
std::optional<bool> PinsInEverySpace() {
#if BUILDFLAG(IS_MAC)
  for (const char* domain : {"dev.serafim.fast", "dev.serafim.fast.dev"}) {
    std::optional<base::DictValue> defaults =
        ReadPlist(base::GetHomeDir()
                      .AppendASCII("Library")
                      .AppendASCII("Preferences")
                      .AppendASCII(base::StrCat({domain, ".plist"})));
    if (defaults) {
      return defaults->FindBool("pins.everySpace").value_or(false);
    }
  }
#endif
  return std::nullopt;
}

}  // namespace

base::FilePath YabSessionPath(const base::FilePath& root,
                              std::string_view space_id) {
  if (base::EqualsCaseInsensitiveASCII(space_id, kFirstSpaceId)) {
    return root.AppendASCII("session.json");
  }
  return root.AppendASCII(base::StrCat({"session-", space_id, ".json"}));
}

base::FilePath YabSharedPinsPath(const base::FilePath& root) {
  return YabSessionPath(root, kSharedPinsId);
}

ImportedSidebar YabSidebar(
    const SourceProfile& profile,
    const base::ListValue* spaces,
    const std::map<std::string, const base::DictValue*>& sessions,
    const base::DictValue* shared_pins,
    std::vector<arc::Item> bookmarks) {
  ImportedSidebar sidebar;
  const std::string prefix = base::StrCat({profile.id, ":"});
  // The first profile has the pins every space shows and the bookmarks.
  const std::string_view key = SourceKey(profile);
  const bool first =
      key.empty() || base::EqualsCaseInsensitiveASCII(key, kFirstSpaceId);
  if (!first) {
    shared_pins = nullptr;
    bookmarks.clear();
  }

  base::ListValue default_spaces;
  if (!spaces) {
    if (sessions.empty() || !first) {
      AddBookmarksSpace(profile, std::move(bookmarks), sidebar);
      return sidebar;
    }
    default_spaces.Append(base::DictValue()
                              .Set("id", kFirstSpaceId)
                              .Set("name", "Personal")
                              .Set("icon", "\xF0\x9F\x8F\xA0"));  // 🏠
    spaces = &default_spaces;
  }

  for (const base::Value& value : *spaces) {
    const base::DictValue* space = value.GetIfDict();
    const std::string id = space ? StringAt(*space, "id") : std::string();
    if (id.empty() || (!key.empty() && !base::EqualsCaseInsensitiveASCII(
                                           ProfileKeyOf(*space), key))) {
      continue;
    }
    arc::Space out;
    out.id = base::StrCat({prefix, "space:", id});
    std::u16string name = base::UTF8ToUTF16(StringAt(*space, "name"));
    out.name = name.empty()
                   ? l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_SPACE)
                   : std::move(name);
    const std::string icon = StringAt(*space, "icon");
    out.icon = AppSpaceIcon(icon, icon);
    ApplyYabPaint(*space, out);
    if (const auto it = sessions.find(id); it != sessions.end()) {
      YabTabs tabs =
          ParseSession(*it->second, base::StrCat({prefix, "space:", id, ":"}));
      out.pinned = std::move(tabs.pins);
      out.pinned.insert(out.pinned.end(),
                        std::make_move_iterator(tabs.kept.begin()),
                        std::make_move_iterator(tabs.kept.end()));
      out.today = std::move(tabs.today);
    }
    sidebar.spaces.push_back(std::move(out));
  }

  if (shared_pins) {
    YabTabs tabs = ParseSession(*shared_pins, base::StrCat({prefix, "pins:"}));
    sidebar.favorites = std::move(tabs.pins);
  }

  if (!bookmarks.empty()) {
    AddBookmarksSpace(profile, std::move(bookmarks), sidebar);
  }
  return sidebar;
}

std::vector<arc::Item> ParseYabBookmarks(const base::ListValue& bookmarks,
                                         std::string_view id_prefix) {
  std::vector<arc::Item> out;
  AddBookmarks(bookmarks, std::string(id_prefix), 0, out);
  return out;
}

std::vector<HistoryEntry> ParseYabHistory(const base::ListValue& history) {
  std::vector<HistoryEntry> out;
  for (const base::Value& value : history) {
    const base::DictValue* visit = value.GetIfDict();
    if (!visit) {
      continue;
    }
    HistoryEntry entry;
    entry.url = GURL(StringAt(*visit, "url"));
    if (!entry.url.is_valid() || !entry.url.SchemeIsHTTPOrHTTPS()) {
      continue;
    }
    entry.title = base::UTF8ToUTF16(StringAt(*visit, "title"));
    entry.visit_count = visit->FindInt("count").value_or(1);
    entry.typed_count = visit->FindInt("typed").value_or(0);
    if (std::optional<double> last = visit->FindDouble("last")) {
      entry.last_visit = arc::FromArcTime(*last);
    }
    out.push_back(std::move(entry));
  }
  return out;
}

std::vector<AppProfile> ParseYabProfiles(const base::ListValue* spaces) {
  std::vector<AppProfile> profiles;
  if (!spaces) {
    AppProfile& first = profiles.emplace_back();
    first.key = std::string(kFirstSpaceId);
    first.spaces.push_back(
        l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_PERSONAL));
    return profiles;
  }
  for (const base::Value& value : *spaces) {
    const base::DictValue* space = value.GetIfDict();
    if (!space || StringAt(*space, "id").empty()) {
      continue;
    }
    const std::string key = ProfileKeyOf(*space);
    auto it = std::ranges::find(profiles, key, &AppProfile::key);
    if (it == profiles.end()) {
      profiles.emplace_back().key = key;
      it = profiles.end() - 1;
    }
    std::u16string name = base::UTF8ToUTF16(StringAt(*space, "name"));
    it->spaces.push_back(
        name.empty() ? l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_SPACE)
                     : std::move(name));
  }
  return profiles;
}

base::FilePath YabChromiumFolder(const base::FilePath& app,
                                 std::string_view key) {
  const base::FilePath chromium = app.AppendASCII("Chromium");
  const bool shared = base::EqualsCaseInsensitiveASCII(key, kFirstSpaceId);
  // Where Yab recorded the profile's folder (ChromiumPersistentProfiles).
  std::optional<base::Value> receipt =
      ReadJsonFile(app.AppendASCII("ChromiumProfiles")
                       .AppendASCII(base::StrCat(
                           {key, shared ? "-shared" : "-named", ".json"})));
  if (receipt && receipt->is_dict() &&
      receipt->GetDict().FindInt("version") == 2) {
    const std::string physical = StringAt(receipt->GetDict(), "physical");
    const base::FilePath folder =
        chromium.AppendASCII(base::StrCat({"space-", physical}));
    if (!physical.empty() && base::DirectoryExists(folder)) {
      return folder;
    }
  }
  return chromium.AppendASCII(base::StrCat({"space-", key}));
}

ImportedSidebar ReadYabSidebar(const SourceProfile& profile,
                               bool import_bookmarks,
                               bool import_tabs) {
  const base::FilePath root = profile.root.DirName();
  std::vector<arc::Item> bookmarks;
  if (import_bookmarks) {
    if (std::optional<base::Value> list =
            ReadJsonFile(root.AppendASCII("bookmarks.json"));
        list && list->is_list()) {
      bookmarks =
          ParseYabBookmarks(list->GetList(), base::StrCat({profile.id, ":"}));
    }
  }
  if (!import_tabs) {
    ImportedSidebar sidebar;
    AddBookmarksSpace(profile, std::move(bookmarks), sidebar);
    return sidebar;
  }

  std::optional<base::Value> spaces =
      ReadJsonFile(root.AppendASCII("spaces.json"));
  const base::ListValue* space_list =
      spaces && spaces->is_list() ? &spaces->GetList() : nullptr;
  std::vector<std::string> ids;
  if (space_list) {
    for (const base::Value& value : *space_list) {
      if (const base::DictValue* space = value.GetIfDict()) {
        ids.push_back(StringAt(*space, "id"));
      }
    }
  } else {
    ids.emplace_back(kFirstSpaceId);
  }
  std::map<std::string, base::DictValue> read;
  for (const std::string& id : ids) {
    if (std::optional<base::DictValue> session =
            ReadDict(YabSessionPath(root, id))) {
      read.emplace(id, std::move(*session));
    }
  }
  std::map<std::string, const base::DictValue*> sessions;
  for (const auto& [id, session] : read) {
    sessions[id] = &session;
  }
  std::optional<base::DictValue> shared_pins;
  if (PinsInEverySpace().value_or(true)) {
    shared_pins = ReadDict(YabSharedPinsPath(root));
  }
  return YabSidebar(profile, space_list, sessions,
                    shared_pins ? &*shared_pins : nullptr,
                    std::move(bookmarks));
}

std::vector<HistoryEntry> ReadYabHistory(const base::FilePath& root,
                                         std::string_view key,
                                         size_t max_entries) {
  std::vector<base::FilePath> paths;
  if (key.empty()) {
    base::FileEnumerator files(root, /*recursive=*/false,
                               base::FileEnumerator::FILES,
                               FILE_PATH_LITERAL("history*.json"));
    for (base::FilePath path = files.Next(); !path.empty();
         path = files.Next()) {
      paths.push_back(path);
    }
  } else if (base::EqualsCaseInsensitiveASCII(key, kFirstSpaceId)) {
    paths.push_back(root.AppendASCII("history.json"));
  } else {
    paths.push_back(root.AppendASCII(base::StrCat({"history-", key, ".json"})));
  }
  std::vector<HistoryEntry> all;
  for (const base::FilePath& path : paths) {
    if (std::optional<base::Value> list = ReadJsonFile(path);
        list && list->is_list()) {
      std::vector<HistoryEntry> entries = ParseYabHistory(list->GetList());
      all.insert(all.end(), std::make_move_iterator(entries.begin()),
                 std::make_move_iterator(entries.end()));
    }
  }
  return MergeHistory(std::move(all), max_entries);
}

}  // namespace compass::browser_import
