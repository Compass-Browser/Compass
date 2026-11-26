// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/crest_reader.h"

#include <algorithm>
#include <set>
#include <utility>

#include "base/files/scoped_temp_dir.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "compass/browser/arc_import/arc_sidebar.h"
#include "compass/browser/browser_import/app_sidebar.h"
#include "compass/strings/grit/compass_strings.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "ui/base/l10n/l10n_util.h"

namespace compass::browser_import {

namespace {

// Folders nest at most 16 deep in Crest; more is a malformed file.
constexpr size_t kMaxDepth = 32;

constexpr std::string_view kHistoryPrefix = "history.";

// One of Crest's IDs: Swift's typed {"rawValue": "<UUID>"}, or the bare
// UUID.
std::string IdAt(const base::DictValue& dict, std::string_view key) {
  const base::Value* value = dict.Find(key);
  if (!value) {
    return std::string();
  }
  if (const base::DictValue* wrapped = value->GetIfDict()) {
    return StringAt(*wrapped, "rawValue");
  }
  const std::string* bare = value->GetIfString();
  return bare ? *bare : std::string();
}

// The profile a space browses with (SpaceState.ProfileId), upper case.
std::string ProfileKeyOf(const base::DictValue& space) {
  std::string key = IdAt(space, "sharedProfileID");
  if (key.empty()) {
    if (const base::DictValue* profile = space.FindDict("profile")) {
      key = IdAt(*profile, "id");
    }
  }
  return base::ToUpperASCII(key);
}

// Crest's accents (SpaceAccent): their swatches, near enough.
std::optional<SpaceColor> AccentColor(std::string_view accent) {
  if (accent == "indigo") {
    return SpaceColor::FromRGB(79, 70, 229);
  }
  if (accent == "orange") {
    return SpaceColor::kOrange;
  }
  if (accent == "teal") {
    return SpaceColor::FromRGB(20, 184, 166);
  }
  if (accent == "rose") {
    return SpaceColor::FromRGB(244, 63, 94);
  }
  return std::nullopt;
}

void ApplyCrestLook(const base::DictValue& space, arc::Space& out) {
  std::vector<SpaceColor> found;
  if (const base::ListValue* colors =
          space.FindListByDottedPath("branding.colors")) {
    for (const base::Value& value : *colors) {
      const base::DictValue* color = value.GetIfDict();
      if (!color) {
        continue;
      }
      const SpaceColor space_color =
          arc::ArcSpaceColor(color->FindDouble("red").value_or(0),
                             color->FindDouble("green").value_or(0),
                             color->FindDouble("blue").value_or(0));
      if (!space_color.is_grey()) {
        found.push_back(space_color);
      }
      if (found.size() == kMaxSpaceGradientColors) {
        break;
      }
    }
  }
  if (found.empty()) {
    if (std::optional<SpaceColor> accent =
            AccentColor(StringAt(space, "accent"))) {
      found.push_back(*accent);
    }
  }
  if (found.empty()) {
    return;
  }
  out.color = found[0];
  out.gradient.assign(found.begin() + 1, found.end());
}

// A space's tabs and folders, built into Pinned and Today.
class CrestSpaceBuilder {
 public:
  CrestSpaceBuilder(const base::DictValue& space, std::string prefix)
      : prefix_(std::move(prefix)),
        pinned_(prefix_),
        saved_(prefix_),
        current_(prefix_) {
    if (const base::ListValue* folders = space.FindList("folders")) {
      for (const base::Value& value : *folders) {
        if (const base::DictValue* folder = value.GetIfDict()) {
          folders_[IdAt(*folder, "id")] = folder;
        }
      }
    }
    if (const base::ListValue* groups = space.FindList("splitGroups")) {
      for (const base::Value& value : *groups) {
        if (const base::DictValue* group = value.GetIfDict()) {
          splits_[IdAt(*group, "id")] = group;
        }
      }
    }
    const base::ListValue* tabs = space.FindList("tabs");
    if (!tabs) {
      return;
    }
    // A split group's tabs, in order, to place together at the first.
    std::map<std::string, std::vector<const base::DictValue*>> split_tabs;
    for (const base::Value& value : *tabs) {
      if (const base::DictValue* tab = value.GetIfDict()) {
        const std::string split = IdAt(*tab, "splitGroupID");
        if (!split.empty()) {
          split_tabs[split].push_back(tab);
        }
      }
    }
    std::set<std::string> placed_splits;
    for (const base::Value& value : *tabs) {
      const base::DictValue* tab = value.GetIfDict();
      if (!tab) {
        continue;
      }
      const std::string placement = StringAt(*tab, "placement");
      const bool kept = placement != "current";
      std::optional<arc::Item> item;
      const std::string split = IdAt(*tab, "splitGroupID");
      if (!split.empty() && split_tabs[split].size() > 1) {
        if (!placed_splits.insert(split).second) {
          continue;
        }
        std::vector<arc::Item> panes;
        for (const base::DictValue* pane : split_tabs[split]) {
          if (std::optional<arc::Item> converted = Convert(*pane, kept)) {
            panes.push_back(std::move(*converted));
          }
        }
        if (panes.size() == 1) {
          item = std::move(panes[0]);
        } else if (panes.size() > 1) {
          std::u16string title;
          if (const auto it = splits_.find(split); it != splits_.end()) {
            title = base::UTF8ToUTF16(StringAt(*it->second, "customTitle"));
          }
          item = MakeFolder(base::StrCat({prefix_, "split:", split}), title,
                            std::move(panes));
          item->kind = arc::Item::Kind::kSplit;
        }
      } else {
        item = Convert(*tab, kept);
      }
      if (!item) {
        continue;
      }
      if (placement == "pinned") {
        pinned_.Add({}, std::move(*item));
      } else {
        (kept ? saved_ : current_)
            .Add(FolderPath(IdAt(*tab, "folderID")), std::move(*item));
      }
    }
  }

  void Build(arc::Space& space) && {
    space.pinned = std::move(pinned_).Build();
    std::vector<arc::Item> saved = std::move(saved_).Build();
    space.pinned.insert(space.pinned.end(),
                        std::make_move_iterator(saved.begin()),
                        std::make_move_iterator(saved.end()));
    space.today = std::move(current_).Build();
  }

 private:
  std::optional<arc::Item> Convert(const base::DictValue& tab,
                                   bool kept) const {
    if (tab.Find("nativeContent")) {
      return std::nullopt;
    }
    const std::string saved = StringAt(tab, "savedURL");
    const GURL url(kept && !saved.empty() ? saved : StringAt(tab, "url"));
    if (!IsImportableTabURL(url)) {
      return std::nullopt;
    }
    std::string title = kept ? StringAt(tab, "savedTitle") : std::string();
    if (title.empty()) {
      title = StringAt(tab, "title");
    }
    base::Time last_active;
    if (std::optional<double> at = tab.FindDouble("lastActivatedAt")) {
      last_active = arc::FromArcTime(*at);
    }
    arc::Item item = MakeTab(base::StrCat({prefix_, "tab:", IdAt(tab, "id")}),
                             url, base::UTF8ToUTF16(title), last_active);
    item.title = base::UTF8ToUTF16(StringAt(tab, "customTitle"));
    return item;
  }

  // The folders |folder| is in, from the top down, itself last.
  std::vector<FolderStep> FolderPath(std::string folder) const {
    std::vector<FolderStep> path;
    std::set<std::string> seen;
    while (!folder.empty() && seen.insert(folder).second &&
           path.size() < kMaxDepth) {
      const auto it = folders_.find(folder);
      if (it == folders_.end()) {
        break;
      }
      path.insert(path.begin(), FolderStep{folder, base::UTF8ToUTF16(StringAt(
                                                       *it->second, "title"))});
      folder = IdAt(*it->second, "parentID");
    }
    return path;
  }

  const std::string prefix_;
  std::map<std::string, const base::DictValue*> folders_;
  std::map<std::string, const base::DictValue*> splits_;
  FolderTreeBuilder pinned_;
  FolderTreeBuilder saved_;
  FolderTreeBuilder current_;
};

}  // namespace

CrestSession::CrestSession() = default;
CrestSession::CrestSession(CrestSession&&) = default;
CrestSession& CrestSession::operator=(CrestSession&&) = default;
CrestSession::~CrestSession() = default;

base::FilePath CrestSessionPath(const base::FilePath& root) {
  return root.AppendASCII("ControlPlane").AppendASCII("session.sqlite");
}

std::optional<CrestSession> ReadCrestSession(const base::FilePath& path,
                                             bool with_history) {
  base::ScopedTempDir temp;
  if (!temp.CreateUniqueTempDir()) {
    return std::nullopt;
  }
  const base::FilePath copy = CopyDatabase(path, temp.GetPath());
  if (copy.empty()) {
    return std::nullopt;
  }
  sql::Database db(sql::DatabaseOptions(), "FirefoxImporter");
  if (!db.Open(copy)) {
    return std::nullopt;
  }
  sql::Statement statement(
      db.GetUniqueStatement("SELECT part, data FROM checkpoint"));
  CrestSession session;
  while (statement.Step()) {
    const std::string part = statement.ColumnString(0);
    const bool history = part.starts_with(kHistoryPrefix);
    if (part != "core" && (!history || !with_history)) {
      continue;
    }
    std::optional<base::Value> value =
        ParseJson(statement.ColumnBlobAsString(1));
    if (!value) {
      continue;
    }
    if (part == "core" && value->is_dict()) {
      session.core = std::move(*value).TakeDict();
    } else if (history && value->is_list()) {
      session.history.emplace(
          base::ToUpperASCII(part.substr(kHistoryPrefix.size())),
          std::move(*value).TakeList());
    }
  }
  if (!session.core) {
    return std::nullopt;
  }
  return session;
}

std::vector<AppProfile> ParseCrestProfiles(const base::DictValue& core) {
  std::vector<AppProfile> profiles;
  const base::ListValue* spaces = core.FindList("spaces");
  if (!spaces) {
    return profiles;
  }
  for (const base::Value& value : *spaces) {
    const base::DictValue* space = value.GetIfDict();
    const std::string key = space ? ProfileKeyOf(*space) : std::string();
    if (key.empty() || IdAt(*space, "id").empty()) {
      continue;
    }
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

std::vector<std::string> CrestSpaceIds(const base::DictValue& core,
                                       std::string_view key) {
  std::vector<std::string> ids;
  if (const base::ListValue* spaces = core.FindList("spaces")) {
    for (const base::Value& value : *spaces) {
      const base::DictValue* space = value.GetIfDict();
      if (space &&
          base::EqualsCaseInsensitiveASCII(ProfileKeyOf(*space), key)) {
        ids.push_back(IdAt(*space, "id"));
      }
    }
  }
  return ids;
}

ImportedSidebar CrestSidebar(const SourceProfile& profile,
                             const base::DictValue& core) {
  ImportedSidebar sidebar;
  const std::string prefix = base::StrCat({profile.id, ":"});
  const base::ListValue* spaces = core.FindList("spaces");
  if (!spaces) {
    return sidebar;
  }
  const std::string_view key = SourceKey(profile);
  for (const base::Value& value : *spaces) {
    const base::DictValue* space = value.GetIfDict();
    const std::string id = space ? IdAt(*space, "id") : std::string();
    if (id.empty() ||
        !base::EqualsCaseInsensitiveASCII(ProfileKeyOf(*space), key)) {
      continue;
    }
    arc::Space out;
    out.id = base::StrCat({prefix, "space:", id});
    std::u16string name = base::UTF8ToUTF16(StringAt(*space, "name"));
    out.name = name.empty()
                   ? l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_SPACE)
                   : std::move(name);
    out.icon = AppSpaceIcon("", StringAt(*space, "symbol"));
    ApplyCrestLook(*space, out);
    CrestSpaceBuilder(*space, base::StrCat({prefix, "space:", id, ":"}))
        .Build(out);
    sidebar.spaces.push_back(std::move(out));
  }
  return sidebar;
}

std::vector<HistoryEntry> ParseCrestHistory(const base::ListValue& history) {
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
    entry.visit_count = visit->FindInt("visitCount").value_or(1);
    if (std::optional<double> last = visit->FindDouble("lastVisitedAt")) {
      entry.last_visit = arc::FromArcTime(*last);
    }
    out.push_back(std::move(entry));
  }
  return out;
}

}  // namespace compass::browser_import
