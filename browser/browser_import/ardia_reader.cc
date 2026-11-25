// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/ardia_reader.h"

#include <map>
#include <set>
#include <string>
#include <utility>

#include "base/memory/raw_ref.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "compass/browser/browser_import/app_sidebar.h"

namespace compass::browser_import {

namespace {

// Groups nest at most this deep (a guard against a malformed file).
constexpr size_t kMaxDepth = 32;

// Where a tab of Ardia's sidebar is.
enum class Section { kTopApps, kPinned, kToday };

// The Chromium profile folder an Ardia profile is in ("default" is
// Default; several profiles may share one through dataId).
std::string ProfileDirFor(const base::DictValue& profile,
                          const std::string& id) {
  // engineProfile(): its data's ID, "default" being Default's.
  std::string data_id = StringAt(profile, "dataId");
  if (data_id.empty()) {
    data_id = id;
  }
  if (data_id == "default") {
    return "Default";
  }
  return base::StrCat({"Profile ", data_id});
}

// Ardia's profile colors (PROFILE_COLOR_NAMES): Compass's palette's names,
// and plum and neutral.
SpaceColor ArdiaColor(std::string_view name) {
  if (name == "plum") {
    return SpaceColor::FromRGB(142, 69, 133);
  }
  for (const NamedSpaceColor& named : kSpaceColorPalette) {
    if (named.name == name) {
      return named.color;
    }
  }
  return HexSpaceColor(name).value_or(SpaceColor::kGrey);
}

// A tab's place, from its pin ({"as": "tile" | "row", "folder"}), or from
// pinned/pinnedAsRow/tileFolder in files before version 5.
Section SectionOf(const base::DictValue& tab, const base::DictValue** folder) {
  *folder = nullptr;
  if (const base::DictValue* pin = tab.FindDict("pin")) {
    if (StringAt(*pin, "as") == "row") {
      return Section::kPinned;
    }
    *folder = pin->FindDict("folder");
    return Section::kTopApps;
  }
  if (tab.FindBool("pinned").value_or(false)) {
    if (tab.FindBool("pinnedAsRow").value_or(false)) {
      return Section::kPinned;
    }
    *folder = tab.FindDict("tileFolder");
    return Section::kTopApps;
  }
  return Section::kToday;
}

// One Ardia profile's sidebar, built as the session's tabs come.
class ArdiaSpaceBuilder {
 public:
  ArdiaSpaceBuilder(const std::string& prefix,
                    const std::map<std::string, const base::DictValue*>& groups)
      : prefix_(prefix),
        groups_(raw_ref(groups)),
        top_apps_(prefix),
        pinned_(prefix),
        today_(prefix) {}

  // |tab| in |section|, in the group |group| (may be empty), its split's
  // other panes |panes| (with their groups ignored: a split stays whole).
  void Add(const base::DictValue& tab,
           Section section,
           const base::DictValue* tile_folder,
           const std::string& group,
           const std::vector<const base::DictValue*>& panes) {
    std::optional<arc::Item> item = Convert(tab, section != Section::kToday);
    if (!item) {
      return;
    }
    if (!panes.empty()) {
      std::vector<arc::Item> children;
      children.push_back(std::move(*item));
      for (const base::DictValue* pane : panes) {
        if (std::optional<arc::Item> converted =
                Convert(*pane, section != Section::kToday)) {
          children.push_back(std::move(*converted));
        }
      }
      if (children.size() == 1) {
        item = std::move(children[0]);
      } else {
        item =
            MakeFolder(base::StrCat({prefix_, "split:", StringAt(tab, "id")}),
                       u"", std::move(children));
        item->kind = arc::Item::Kind::kSplit;
      }
    }
    switch (section) {
      case Section::kTopApps: {
        std::vector<FolderStep> path;
        if (tile_folder) {
          path.push_back(
              FolderStep{base::StrCat({"tile:", StringAt(*tile_folder, "id")}),
                         base::UTF8ToUTF16(StringAt(*tile_folder, "name"))});
        }
        top_apps_.Add(path, std::move(*item));
        break;
      }
      case Section::kPinned:
        pinned_.Add(GroupPath(group), std::move(*item));
        break;
      case Section::kToday:
        today_.Add(GroupPath(group), std::move(*item));
        break;
    }
  }

  void Build(arc::Space& space) && {
    space.pinned = std::move(top_apps_).Build();
    std::vector<arc::Item> pinned = std::move(pinned_).Build();
    space.pinned.insert(space.pinned.end(),
                        std::make_move_iterator(pinned.begin()),
                        std::make_move_iterator(pinned.end()));
    space.today = std::move(today_).Build();
  }

 private:
  std::optional<arc::Item> Convert(const base::DictValue& tab,
                                   bool pinned) const {
    const std::string pinned_url = StringAt(tab, "pinnedUrl");
    const GURL url(pinned && !pinned_url.empty() ? pinned_url
                                                 : StringAt(tab, "url"));
    if (!IsImportableTabURL(url)) {
      return std::nullopt;
    }
    base::Time last_active;
    if (std::optional<double> ms = tab.FindDouble("lastActiveAt")) {
      last_active = base::Time::FromMillisecondsSinceUnixEpoch(*ms);
    }
    arc::Item item =
        MakeTab(base::StrCat({prefix_, "tab:", StringAt(tab, "id")}), url,
                base::UTF8ToUTF16(StringAt(tab, "title")), last_active);
    item.title = base::UTF8ToUTF16(StringAt(tab, "customTitle"));
    item.muted = tab.FindBool("muted").value_or(false);
    return item;
  }

  // The groups |group| is in, from the top down, itself last.
  std::vector<FolderStep> GroupPath(const std::string& group) const {
    std::vector<FolderStep> path;
    std::set<std::string> seen;
    std::string id = group;
    while (!id.empty() && seen.insert(id).second && path.size() < kMaxDepth) {
      const auto it = groups_->find(id);
      if (it == groups_->end()) {
        break;
      }
      path.insert(path.begin(),
                  FolderStep{base::StrCat({"group:", id}),
                             base::UTF8ToUTF16(StringAt(*it->second, "name"))});
      id = StringAt(*it->second, "parentId");
    }
    return path;
  }

  const std::string prefix_;
  const raw_ref<const std::map<std::string, const base::DictValue*>> groups_;
  FolderTreeBuilder top_apps_;
  FolderTreeBuilder pinned_;
  FolderTreeBuilder today_;
};

}  // namespace

std::vector<base::FilePath> ArdiaSessionPaths(const SourceProfile& profile) {
#if BUILDFLAG(IS_LINUX)
  return {profile.path.AppendASCII("Ardia").AppendASCII("linux-session.json"),
          profile.root.AppendASCII("Default").AppendASCII("Ardia").AppendASCII(
              "linux-session.json")};
#else
  return {profile.root.DirName().AppendASCII("session.json")};
#endif
}

ImportedSidebar ArdiaSidebar(const SourceProfile& profile,
                             const base::DictValue& session,
                             std::vector<arc::Item> bookmarks,
                             bool import_tabs) {
  ImportedSidebar sidebar;
  const std::string prefix = base::StrCat({profile.id, ":"});
  const std::string profile_dir = profile.path.BaseName().AsUTF8Unsafe();
  const base::DictValue* profiles = session.FindDict("profiles");

  if (import_tabs && profiles) {
    // The Ardia profiles in this folder, in Ardia's order.
    std::vector<std::string> ids;
    std::set<std::string> placed;
    auto consider = [&](const std::string& id) {
      const base::DictValue* ardia_profile = profiles->FindDict(id);
      if (ardia_profile && !placed.contains(id) &&
          base::EqualsCaseInsensitiveASCII(ProfileDirFor(*ardia_profile, id),
                                           profile_dir)) {
        placed.insert(id);
        ids.push_back(id);
      }
    };
    if (const base::ListValue* order = session.FindList("profileOrder")) {
      for (const base::Value& value : *order) {
        if (const std::string* id = value.GetIfString()) {
          consider(*id);
        }
      }
    }
    for (const auto [id, value] : *profiles) {
      consider(id);
    }

    // Tabs, groups and splits by ID.
    std::map<std::string, const base::DictValue*> tabs;
    if (const base::ListValue* list = session.FindList("tabs")) {
      for (const base::Value& value : *list) {
        if (const base::DictValue* tab = value.GetIfDict()) {
          tabs[StringAt(*tab, "id")] = tab;
        }
      }
    }
    std::map<std::string, const base::DictValue*> groups;
    std::map<std::string, std::string> group_of;
    if (const base::ListValue* list = session.FindList("groups")) {
      for (const base::Value& value : *list) {
        const base::DictValue* group = value.GetIfDict();
        if (!group) {
          continue;
        }
        const std::string id = StringAt(*group, "id");
        groups[id] = group;
        if (const base::ListValue* members = group->FindList("tabIds")) {
          for (const base::Value& member : *members) {
            if (const std::string* tab_id = member.GetIfString()) {
              group_of[*tab_id] = id;
            }
          }
        }
      }
    }
    // A split's panes, by each pane's tab ID.
    std::map<std::string, std::vector<std::string>> split_of;
    auto add_splits = [&](const base::ListValue* list) {
      if (!list) {
        return;
      }
      for (const base::Value& value : *list) {
        const base::DictValue* split = value.GetIfDict();
        const base::ListValue* slots =
            split ? split->FindList("slots") : nullptr;
        if (!slots) {
          continue;
        }
        std::vector<std::string> panes;
        for (const base::Value& slot : *slots) {
          if (const base::DictValue* dict = slot.GetIfDict()) {
            if (const std::string* tab_id = dict->FindString("tabId")) {
              panes.push_back(*tab_id);
            }
            if (const base::ListValue* stack = dict->FindList("stack")) {
              for (const base::Value& pane : *stack) {
                if (const std::string* tab_id = pane.GetIfString()) {
                  panes.push_back(*tab_id);
                }
              }
            }
          }
        }
        if (panes.size() > 1) {
          for (const std::string& pane : panes) {
            split_of[pane] = panes;
          }
        }
      }
    };
    add_splits(session.FindList("splits"));

    // Windows in Ardia's order.
    std::vector<const base::DictValue*> windows;
    {
      std::map<std::string, const base::DictValue*> by_id;
      std::vector<std::string> listed;
      if (const base::ListValue* list = session.FindList("windows")) {
        for (const base::Value& value : *list) {
          if (const base::DictValue* window = value.GetIfDict()) {
            const std::string id = StringAt(*window, "id");
            if (by_id.emplace(id, window).second) {
              listed.push_back(id);
            }
          }
        }
      }
      std::set<std::string> taken;
      if (const base::ListValue* order = session.FindList("windowOrder")) {
        for (const base::Value& value : *order) {
          const std::string* id = value.GetIfString();
          if (id && by_id.contains(*id) && taken.insert(*id).second) {
            windows.push_back(by_id[*id]);
          }
        }
      }
      for (const std::string& id : listed) {
        if (taken.insert(id).second) {
          windows.push_back(by_id[id]);
        }
      }
    }

    for (const std::string& ardia_id : ids) {
      const base::DictValue& ardia_profile = *profiles->FindDict(ardia_id);
      arc::Space space;
      space.id = base::StrCat({prefix, "space:", ardia_id});
      std::u16string name = base::UTF8ToUTF16(StringAt(ardia_profile, "name"));
      space.name = name.empty() ? SpaceNameFor(profile) : std::move(name);
      space.color = ArdiaColor(StringAt(ardia_profile, "color"));
      space.icon = AppSpaceIcon(StringAt(ardia_profile, "icon"), "");

      const std::string space_prefix =
          base::StrCat({prefix, "space:", ardia_id, ":"});
      // Groups of its windows, and of its parked pins.
      const base::DictValue* parked =
          session.FindDictByDottedPath("parkedPins");
      const base::DictValue* parked_pins =
          parked ? parked->FindDict(ardia_id) : nullptr;
      std::map<std::string, const base::DictValue*> space_groups = groups;
      if (const base::ListValue* list =
              parked_pins ? parked_pins->FindList("groups") : nullptr) {
        for (const base::Value& value : *list) {
          if (const base::DictValue* group = value.GetIfDict()) {
            space_groups.try_emplace(StringAt(*group, "id"), group);
          }
        }
      }
      ArdiaSpaceBuilder builder(space_prefix, space_groups);
      // Pins are copied into every window showing the profile, linked by
      // pinKey: each comes once.
      std::set<std::string> pins_seen;
      std::set<std::string> done;
      auto add_tab = [&](const base::DictValue& tab, const std::string& group,
                         bool from_window) {
        const std::string id = StringAt(tab, "id");
        if (done.contains(id)) {
          return;
        }
        const base::DictValue* tile_folder = nullptr;
        const Section section = SectionOf(tab, &tile_folder);
        if (section != Section::kToday) {
          const std::string key = StringAt(tab, "pinKey");
          if (!pins_seen.insert(key.empty() ? id : key).second) {
            return;
          }
        }
        std::vector<const base::DictValue*> panes;
        if (const auto it = split_of.find(id);
            from_window && it != split_of.end()) {
          for (const std::string& pane : it->second) {
            const auto tab_it = tabs.find(pane);
            if (pane == id || done.contains(pane) || tab_it == tabs.end()) {
              continue;
            }
            panes.push_back(tab_it->second);
            done.insert(pane);
          }
        }
        done.insert(id);
        builder.Add(tab, section, tile_folder, group, panes);
      };

      for (const base::DictValue* window : windows) {
        const base::ListValue* tab_ids = window->FindList("tabIds");
        if (!tab_ids || StringAt(*window, "kind") == "private" ||
            window->FindBool("incognito").value_or(false)) {
          continue;
        }
        const std::string window_profile = StringAt(*window, "profileId");
        for (const base::Value& value : *tab_ids) {
          const std::string* tab_id = value.GetIfString();
          const auto it = tab_id ? tabs.find(*tab_id) : tabs.end();
          if (it == tabs.end()) {
            continue;
          }
          std::string tab_profile = StringAt(*it->second, "profileId");
          if ((tab_profile.empty() ? window_profile : tab_profile) !=
              ardia_id) {
            continue;
          }
          const auto group = group_of.find(*tab_id);
          add_tab(*it->second,
                  group == group_of.end() ? std::string() : group->second,
                  /*from_window=*/true);
        }
      }

      // Pins of the profile while no window shows it.
      if (const base::ListValue* list =
              parked_pins ? parked_pins->FindList("tabs") : nullptr) {
        for (const base::Value& value : *list) {
          const base::DictValue* tab = value.GetIfDict();
          if (!tab) {
            continue;
          }
          const std::string id = StringAt(*tab, "id");
          const base::DictValue* tile_folder = nullptr;
          const Section section = SectionOf(*tab, &tile_folder);
          const std::string key = StringAt(*tab, "pinKey");
          if (section == Section::kToday || done.contains(id) ||
              !pins_seen.insert(key.empty() ? id : key).second) {
            continue;
          }
          done.insert(id);
          builder.Add(*tab, section, tile_folder, StringAt(*tab, "groupId"),
                      {});
        }
      }

      std::move(builder).Build(space);
      sidebar.spaces.push_back(std::move(space));
    }
  }

  AddBookmarksSpace(profile, std::move(bookmarks), sidebar);
  return sidebar;
}

}  // namespace compass::browser_import
