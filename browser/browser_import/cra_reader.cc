// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/cra_reader.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "compass/browser/browser_import/app_sidebar.h"
#include "compass/strings/grit/compass_strings.h"
#include "ui/base/l10n/l10n_util.h"

namespace compass::browser_import {

namespace {

// The container of favorites Cra shares across a profile's spaces names
// this space.
constexpr std::string_view kSharedSpaceId =
    "00000000-0000-0000-0000-000000000000";

// Folders nest at most this deep (a guard against a malformed file).
constexpr int kMaxDepth = 32;

// The payload of one of Cra's enum cases, as Swift encodes it:
// {"tab": {"_0": {...}}}, or {"folder": {}} for one without.
const base::DictValue* CasePayload(const base::DictValue& data,
                                   std::string_view kind) {
  const base::DictValue* value = data.FindDict(kind);
  if (!value) {
    return nullptr;
  }
  const base::DictValue* payload = value->FindDict("_0");
  return payload ? payload : value;
}

// The Chromium profile folder a space's profile is in.
std::string ProfileDirFor(const base::DictValue& space) {
  const std::string id = StringAt(space, "profileID");
  return id.empty() ? std::string("Default") : base::StrCat({"Profile ", id});
}

class CraTree {
 public:
  CraTree(const base::DictValue& document, std::string id_prefix)
      : id_prefix_(std::move(id_prefix)) {
    if (const base::ListValue* items = document.FindList("items")) {
      for (const base::Value& value : *items) {
        if (const base::DictValue* item = value.GetIfDict()) {
          const std::string id = StringAt(*item, "id");
          if (!id.empty()) {
            items_[id] = item;
          }
        }
      }
    }
  }

  // The space ID a container names, or nullopt if |id| isn't a container.
  std::optional<std::string> ContainerSpace(const std::string& id) const {
    const base::DictValue* item = Find(id);
    const base::DictValue* data = item ? item->FindDict("data") : nullptr;
    const base::DictValue* container =
        data ? CasePayload(*data, "container") : nullptr;
    if (!container) {
      return std::nullopt;
    }
    for (const char* kind : {"favorites", "pinned", "today"}) {
      if (const base::DictValue* root = container->FindDict(kind)) {
        return StringAt(*root, "spaceID");
      }
    }
    return std::nullopt;
  }

  // The entries under the item |id| (a container, folder or split).
  // |pinned|: whether they're kept tabs (Pinned, favorites), which open at
  // the page they were pinned at.
  std::vector<arc::Item> Children(const std::string& id,
                                  bool pinned,
                                  int depth = 0) const {
    std::vector<arc::Item> out;
    const base::DictValue* item = Find(id);
    const base::ListValue* children =
        item ? item->FindList("children") : nullptr;
    if (!children || depth > kMaxDepth) {
      return out;
    }
    for (const base::Value& child : *children) {
      if (const std::string* child_id = child.GetIfString()) {
        if (std::optional<arc::Item> converted =
                Convert(*child_id, pinned, depth + 1)) {
          out.push_back(std::move(*converted));
        }
      }
    }
    return out;
  }

 private:
  const base::DictValue* Find(const std::string& id) const {
    const auto it = items_.find(id);
    return it == items_.end() ? nullptr : it->second;
  }

  std::optional<arc::Item> Convert(const std::string& id,
                                   bool pinned,
                                   int depth) const {
    const base::DictValue* item = Find(id);
    const base::DictValue* data = item ? item->FindDict("data") : nullptr;
    if (!data) {
      return std::nullopt;
    }
    const std::u16string title = base::UTF8ToUTF16(StringAt(*item, "title"));
    const std::string item_id = base::StrCat({id_prefix_, "item:", id});
    if (const base::DictValue* tab = CasePayload(*data, "tab")) {
      // A pinned tab opens at its saved page; another where it was.
      const std::string saved = StringAt(*tab, "savedURL");
      const std::string current = StringAt(*tab, "url");
      const GURL url((pinned && !saved.empty()) || current.empty() ? saved
                                                                   : current);
      if (!IsImportableTabURL(url)) {
        return std::nullopt;
      }
      std::string saved_title = StringAt(*tab, "homeTitle");
      if (!pinned || saved_title.empty()) {
        saved_title = StringAt(*tab, "savedTitle");
      }
      arc::Item out =
          MakeTab(item_id, url, base::UTF8ToUTF16(saved_title),
                  TimeFromIso8601(StringAt(*tab, "timeLastActiveAt")));
      out.title = title;
      out.muted = tab->FindBool("muted").value_or(false);
      return out;
    }
    if (const base::DictValue* split = CasePayload(*data, "split")) {
      std::vector<arc::Item> panes = Children(id, pinned, depth);
      if (panes.empty()) {
        return std::nullopt;
      }
      if (panes.size() == 1) {
        return std::move(panes[0]);
      }
      arc::Item out = MakeFolder(item_id, title, std::move(panes));
      out.kind = arc::Item::Kind::kSplit;
      out.last_active = TimeFromIso8601(StringAt(*split, "timeLastActiveAt"));
      return out;
    }
    if (data->FindDict("folder")) {
      std::vector<arc::Item> children = Children(id, pinned, depth);
      if (children.empty()) {
        return std::nullopt;
      }
      return MakeFolder(
          item_id,
          title.empty() ? l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_FOLDER)
                        : title,
          std::move(children));
    }
    return std::nullopt;
  }

  const std::string id_prefix_;
  std::map<std::string, const base::DictValue*> items_;
};

void ApplyCraTheme(const base::DictValue& space, arc::Space& out) {
  const base::DictValue* theme = space.FindDict("theme");
  if (!theme) {
    return;
  }
  std::optional<SpaceColor> color = HexSpaceColor(StringAt(*theme, "accent"));
  if (!color) {
    if (const base::ListValue* colors = theme->FindList("colors");
        colors && !colors->empty() && (*colors)[0].is_string()) {
      color = HexSpaceColor((*colors)[0].GetString());
    }
  }
  if (!color || color->is_grey()) {
    return;
  }
  out.color = *color;
  if (std::optional<double> intensity = theme->FindDouble("intensity")) {
    out.intensity = std::clamp(*intensity, 0.0, 1.0);
  }
  if (std::optional<double> noise = theme->FindDouble("noise")) {
    out.grain = std::clamp(*noise, 0.0, 1.0);
  }
}

}  // namespace

base::FilePath CraSidebarPath(const base::FilePath& user_data) {
  return user_data.DirName().AppendASCII("sidebar.json");
}

ImportedSidebar CraSidebar(const SourceProfile& profile,
                           const base::DictValue& document,
                           std::vector<arc::Item> bookmarks,
                           bool import_tabs) {
  ImportedSidebar sidebar;
  const std::string prefix = base::StrCat({profile.id, ":"});
  const std::string profile_dir = profile.path.BaseName().AsUTF8Unsafe();

  if (import_tabs) {
    // Spaces in Cra's order: spaceOrder, then any it leaves out.
    std::map<std::string, const base::DictValue*> by_id;
    std::vector<std::string> order;
    if (const base::ListValue* spaces = document.FindList("spaces")) {
      for (const base::Value& value : *spaces) {
        if (const base::DictValue* space = value.GetIfDict()) {
          const std::string id = StringAt(*space, "id");
          if (!id.empty() && by_id.emplace(id, space).second) {
            order.push_back(id);
          }
        }
      }
    }
    std::vector<std::string> ordered;
    std::set<std::string> placed;
    if (const base::ListValue* space_order = document.FindList("spaceOrder")) {
      for (const base::Value& value : *space_order) {
        const std::string* id = value.GetIfString();
        if (id && by_id.contains(*id) && placed.insert(*id).second) {
          ordered.push_back(*id);
        }
      }
    }
    for (const std::string& id : order) {
      if (placed.insert(id).second) {
        ordered.push_back(id);
      }
    }

    const CraTree tree(document, prefix);
    std::set<std::string> shared_favorites;
    for (const std::string& id : ordered) {
      const base::DictValue& space = *by_id[id];
      if (!base::EqualsCaseInsensitiveASCII(ProfileDirFor(space),
                                            profile_dir) ||
          !StringAt(space, "standaloneWindowID").empty()) {
        continue;
      }
      arc::Space out;
      out.id = base::StrCat({prefix, "space:", id});
      std::u16string name = base::UTF8ToUTF16(StringAt(space, "title"));
      out.name = name.empty()
                     ? l10n_util::GetStringUTF16(IDS_COMPASS_IMPORT_SPACE)
                     : std::move(name);
      out.icon =
          AppSpaceIcon(StringAt(space, "emoji"), StringAt(space, "symbol"));
      ApplyCraTheme(space, out);
      out.pinned = tree.Children(StringAt(space, "pinnedContainerID"),
                                 /*pinned=*/true);
      out.today = tree.Children(StringAt(space, "todayContainerID"),
                                /*pinned=*/false);
      const std::string favorites = StringAt(space, "favoritesContainerID");
      if (tree.ContainerSpace(favorites) == kSharedSpaceId) {
        // Shared by the profile's spaces: Compass's Favorites, once.
        if (shared_favorites.insert(favorites).second) {
          std::vector<arc::Item> items =
              tree.Children(favorites, /*pinned=*/true);
          sidebar.favorites.insert(sidebar.favorites.end(),
                                   std::make_move_iterator(items.begin()),
                                   std::make_move_iterator(items.end()));
        }
      } else {
        // The space's own: first in its Pinned.
        std::vector<arc::Item> items =
            tree.Children(favorites, /*pinned=*/true);
        out.pinned.insert(out.pinned.begin(),
                          std::make_move_iterator(items.begin()),
                          std::make_move_iterator(items.end()));
      }
      sidebar.spaces.push_back(std::move(out));
    }
  }

  AddBookmarksSpace(profile, std::move(bookmarks), sidebar);
  return sidebar;
}

}  // namespace compass::browser_import
