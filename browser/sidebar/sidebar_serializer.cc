// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/sidebar_serializer.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json/values_util.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "url/gurl.h"

namespace compass {

namespace {

constexpr int kVersion = 2;
constexpr int kSingleSpaceVersion = 1;

constexpr char kVersionKey[] = "version";
constexpr char kActiveKey[] = "active";
constexpr char kSpacesKey[] = "spaces";
constexpr char kFavoritesKey[] = "favorites";
constexpr char kIdKey[] = "id";
constexpr char kNameKey[] = "name";
constexpr char kColorKey[] = "color";
constexpr char kPinnedKey[] = "pinned";
constexpr char kTodayKey[] = "today";
constexpr char kTypeKey[] = "type";
constexpr char kTitleKey[] = "title";
constexpr char kUrlKey[] = "url";
constexpr char kChildrenKey[] = "children";
constexpr char kLastActiveKey[] = "last_active";
constexpr char kCollapsedKey[] = "collapsed";
constexpr char kCustomTitleKey[] = "custom_title";
constexpr char kArchiveAfterKey[] = "archive_after_hours";
constexpr char kArchiveKey[] = "archive";
constexpr char kArchivedAtKey[] = "archived";
constexpr char kTabsKey[] = "tabs";

// By SpaceColor value.
constexpr auto kColorNames = std::to_array<std::string_view>({
    "grey", "blue", "purple", "pink", "red", "orange", "yellow", "green",
});
static_assert(kColorNames.size() ==
              static_cast<size_t>(SpaceColor::kMaxValue) + 1);

constexpr char kTab[] = "tab";
constexpr char kFolder[] = "folder";
constexpr char kSplit[] = "split";

// Returns null for a node that isn't saved: in Today (|today|), a tab that
// isn't restorable, or a split with no such pane.
std::optional<base::DictValue> SerializeNode(const SidebarNode& node,
                                             bool today) {
  base::DictValue dict;
  dict.Set(kIdKey, node.id().AsLowercaseString());
  switch (node.type()) {
    case SidebarNode::Type::kTab:
      if (today && !IsRestorableURL(node.url())) {
        return std::nullopt;
      }
      dict.Set(kTypeKey, kTab);
      dict.Set(kUrlKey, node.url().spec());
      if (!node.last_active().is_null()) {
        dict.Set(kLastActiveKey, base::TimeToValue(node.last_active()));
      }
      if (!node.custom_title().empty()) {
        dict.Set(kCustomTitleKey, base::UTF16ToUTF8(node.custom_title()));
      }
      break;
    case SidebarNode::Type::kFolder:
      dict.Set(kTypeKey, kFolder);
      if (node.collapsed()) {
        dict.Set(kCollapsedKey, true);
      }
      break;
    case SidebarNode::Type::kSplit:
      dict.Set(kTypeKey, kSplit);
      break;
    case SidebarNode::Type::kRoot:
      NOTREACHED();
  }
  dict.Set(kTitleKey, base::UTF16ToUTF8(node.title()));
  base::ListValue children;
  for (const auto& child : node.children()) {
    if (std::optional<base::DictValue> child_dict =
            SerializeNode(*child, today)) {
      children.Append(std::move(*child_dict));
    }
  }
  if (node.type() == SidebarNode::Type::kSplit && children.empty()) {
    return std::nullopt;
  }
  if (!children.empty()) {
    dict.Set(kChildrenKey, std::move(children));
  }
  return dict;
}

base::ListValue SerializeSection(const SidebarNode& root, bool today) {
  base::ListValue entries;
  for (const auto& node : root.children()) {
    if (std::optional<base::DictValue> dict = SerializeNode(*node, today)) {
      entries.Append(std::move(*dict));
    }
  }
  return entries;
}

// A tab's saved state besides its URL and title.
void SetTabState(const base::DictValue& dict,
                 SpaceModel& model,
                 SidebarNode* tab) {
  if (const base::Value* value = dict.Find(kLastActiveKey)) {
    if (std::optional<base::Time> time = base::ValueToTime(*value)) {
      model.SetLastActive(tab, *time);
    }
  }
  if (const std::string* custom_title = dict.FindString(kCustomTitleKey)) {
    model.SetCustomTitle(tab, base::UTF8ToUTF16(*custom_title));
  }
}

// The saved ID of |dict|, unless it's missing, invalid or already taken in
// |model| (then the node gets a fresh one).
base::Uuid SavedId(const base::DictValue& dict, SpaceModel& model) {
  const std::string* id = dict.FindString(kIdKey);
  base::Uuid uuid = id ? base::Uuid::ParseLowercase(*id) : base::Uuid();
  if (!uuid.is_valid() || model.FindById(uuid)) {
    return base::Uuid();
  }
  return uuid;
}

// Adds the tab |dict| to |parent| at |index|.
SidebarNode* AddSavedTab(const base::DictValue& dict,
                         SpaceModel& model,
                         SidebarNode* parent,
                         size_t index) {
  const std::string* url = dict.FindString(kUrlKey);
  const std::string* title = dict.FindString(kTitleKey);
  SidebarNode* tab = model.AddTab(parent, index,
                                  GURL(url ? *url : std::string()),
                                  title ? base::UTF8ToUTF16(*title) : u"",
                                  SavedId(dict, model));
  SetTabState(dict, model, tab);
  return tab;
}

// Adds |dict| as a child of |parent| at |index|. Returns false if it was
// skipped.
bool DeserializeNode(const base::DictValue& dict,
                     SpaceModel& model,
                     SidebarNode* parent,
                     size_t index) {
  const std::string* type = dict.FindString(kTypeKey);
  const std::string* title = dict.FindString(kTitleKey);
  std::u16string title16 = title ? base::UTF8ToUTF16(*title) : u"";
  if (!type) {
    return false;
  }

  if (*type == kTab) {
    if (parent->type() == SidebarNode::Type::kSplit &&
        parent->children().size() >= kMaxSplitPanes) {
      return false;
    }
    AddSavedTab(dict, model, parent, index);
    return true;
  }

  const base::ListValue* children = dict.FindList(kChildrenKey);
  if (*type == kFolder) {
    if (parent->type() == SidebarNode::Type::kSplit) {
      return false;
    }
    SidebarNode* folder = model.AddFolder(parent, index, std::move(title16),
                                          SavedId(dict, model));
    model.SetCollapsed(folder, dict.FindBool(kCollapsedKey).value_or(false));
    if (children) {
      for (const base::Value& child : *children) {
        if (child.is_dict()) {
          DeserializeNode(child.GetDict(), model, folder,
                          folder->children().size());
        }
      }
    }
    return true;
  }

  if (*type == kSplit) {
    // A split with fewer than two panes is stored as its pane. The split and
    // its panes keep their IDs.
    if (parent->type() == SidebarNode::Type::kSplit || !children) {
      return false;
    }
    std::vector<const base::DictValue*> panes;
    for (const base::Value& child : *children) {
      const base::DictValue* pane = child.GetIfDict();
      const std::string* pane_type =
          pane ? pane->FindString(kTypeKey) : nullptr;
      if (!pane_type || *pane_type != kTab || panes.size() == kMaxSplitPanes) {
        continue;
      }
      panes.push_back(pane);
    }
    if (panes.empty()) {
      return false;
    }
    if (panes.size() == 1) {
      AddSavedTab(*panes[0], model, parent, index);
      return true;
    }
    SidebarNode* split = model.AddSplit(parent, index, SavedId(dict, model));
    for (const base::DictValue* pane : panes) {
      AddSavedTab(*pane, model, split, split->children().size());
    }
    return true;
  }
  return false;
}

// Inserts the entries of |data|'s |key| section into |root|, ahead of its own.
size_t DeserializeSection(const base::DictValue& data,
                          const char* key,
                          SpaceModel& model,
                          SidebarNode* root) {
  const base::ListValue* entries = data.FindList(key);
  if (!entries) {
    return 0;
  }
  size_t inserted = 0;
  for (const base::Value& entry : *entries) {
    if (entry.is_dict() &&
        DeserializeNode(entry.GetDict(), model, root, inserted)) {
      ++inserted;
    }
  }
  return inserted;
}

base::ListValue SerializeArchive(const SpaceModel& model) {
  base::ListValue entries;
  for (const ArchivedEntry& entry : model.archive()) {
    base::ListValue tabs;
    for (const ArchivedEntry::Tab& tab : entry.tabs) {
      tabs.Append(base::DictValue()
                      .Set(kTitleKey, base::UTF16ToUTF8(tab.title))
                      .Set(kUrlKey, tab.url.spec()));
    }
    entries.Append(base::DictValue()
                       .Set(kArchivedAtKey, base::TimeToValue(entry.archived_at))
                       .Set(kTabsKey, std::move(tabs)));
  }
  return entries;
}

void DeserializeArchive(const base::DictValue& data, SpaceModel& model) {
  const base::ListValue* entries = data.FindList(kArchiveKey);
  if (!entries) {
    return;
  }
  for (const base::Value& value : *entries) {
    const base::DictValue* dict = value.GetIfDict();
    const base::ListValue* tabs = dict ? dict->FindList(kTabsKey) : nullptr;
    if (!tabs) {
      continue;
    }
    ArchivedEntry entry;
    if (const base::Value* archived_at = dict->Find(kArchivedAtKey)) {
      entry.archived_at = base::ValueToTime(*archived_at).value_or(base::Time());
    }
    for (const base::Value& tab : *tabs) {
      const base::DictValue* tab_dict = tab.GetIfDict();
      const std::string* url = tab_dict ? tab_dict->FindString(kUrlKey) : nullptr;
      if (!url || entry.tabs.size() == kMaxSplitPanes) {
        continue;
      }
      const std::string* title = tab_dict->FindString(kTitleKey);
      entry.tabs.push_back(
          {title ? base::UTF8ToUTF16(*title) : u"", GURL(*url)});
    }
    model.AddArchived(std::move(entry));
  }
}

}  // namespace

base::DictValue SerializeSpace(const SpaceModel& model) {
  base::DictValue data;
  data.Set(kIdKey, model.id().AsLowercaseString());
  data.Set(kNameKey, base::UTF16ToUTF8(model.name()));
  data.Set(kColorKey, kColorNames[static_cast<size_t>(model.color())]);
  data.Set(kArchiveAfterKey, model.archive_after().InHours());
  data.Set(kPinnedKey, SerializeSection(*model.pinned_root(), false));
  data.Set(kTodayKey, SerializeSection(*model.today_root(), true));
  data.Set(kArchiveKey, SerializeArchive(model));
  return data;
}

size_t DeserializeSpace(const base::DictValue& data, SpaceModel& model) {
  DeserializeArchive(data, model);
  return DeserializeSection(data, kPinnedKey, model, model.pinned_root()) +
         DeserializeSection(data, kTodayKey, model, model.today_root());
}

base::DictValue SerializeSpaceList(const SpaceList& list) {
  base::ListValue spaces;
  for (const auto& space : list.spaces()) {
    spaces.Append(SerializeSpace(*space));
  }
  base::DictValue data;
  data.Set(kVersionKey, kVersion);
  data.Set(kActiveKey, list.active_space().id().AsLowercaseString());
  data.Set(kSpacesKey, std::move(spaces));
  data.Set(kFavoritesKey,
           SerializeSection(*list.favorites().pinned_root(), false));
  return data;
}

size_t DeserializeSpaceList(const base::DictValue& data, SpaceList& list) {
  std::vector<const base::DictValue*> saved;
  std::string active_id;
  const std::optional<int> version = data.FindInt(kVersionKey);
  if (version == kSingleSpaceVersion) {
    saved.push_back(&data);
  } else if (version == kVersion) {
    if (const base::ListValue* spaces = data.FindList(kSpacesKey)) {
      for (const base::Value& space : *spaces) {
        if (const base::DictValue* dict = space.GetIfDict()) {
          saved.push_back(dict);
        }
      }
    }
    if (const std::string* active = data.FindString(kActiveKey)) {
      active_id = *active;
    }
  }
  if (saved.empty()) {
    return 0;
  }

  // Ahead of the favorites already there. Only tabs.
  if (const base::ListValue* favorites = data.FindList(kFavoritesKey)) {
    SpaceModel& model = list.favorites();
    size_t inserted = 0;
    for (const base::Value& entry : *favorites) {
      const base::DictValue* dict = entry.GetIfDict();
      const std::string* type = dict ? dict->FindString(kTypeKey) : nullptr;
      if (type && *type == kTab &&
          DeserializeNode(*dict, model, model.pinned_root(), inserted)) {
        ++inserted;
      }
    }
  }

  size_t active_index = 0;
  for (size_t i = 0; i < saved.size(); ++i) {
    const std::string* id = saved[i]->FindString(kIdKey);
    if (id && *id == active_id) {
      active_index = i;
    }
  }

  SpaceModel& active = list.active_space();
  const size_t active_position = std::distance(
      list.spaces().begin(),
      std::ranges::find(list.spaces(), &active, &std::unique_ptr<SpaceModel>::get));
  for (size_t i = 0; i < saved.size(); ++i) {
    const std::string* id_string = saved[i]->FindString(kIdKey);
    const base::Uuid id = id_string ? base::Uuid::ParseLowercase(*id_string)
                                    : base::Uuid();
    const std::string* name = saved[i]->FindString(kNameKey);
    std::u16string name16 = name ? base::UTF8ToUTF16(*name) : u"";
    SpaceModel* space = nullptr;
    if (i == active_index) {
      space = &active;
      if (id.is_valid() && !list.FindSpace(id)) {
        space->set_id(id);
      }
      // Version 1 files have no names.
      if (!name16.empty()) {
        space->SetName(std::move(name16));
      }
    } else {
      // Saved spaces before the active one go before it.
      const size_t index =
          i < active_index ? active_position + i : list.spaces().size();
      const bool fresh_id = id.is_valid() && !list.FindSpace(id);
      space = list.AddSpace(index, std::move(name16),
                            fresh_id ? std::optional<base::Uuid>(id)
                                     : std::nullopt);
    }
    // Unknown colors (from a later version) stay the default.
    if (const std::string* color = saved[i]->FindString(kColorKey)) {
      for (size_t c = 0; c < kColorNames.size(); ++c) {
        if (*color == kColorNames[c]) {
          space->SetColor(static_cast<SpaceColor>(c));
        }
      }
    }
    // Older files have none: the default applies.
    if (std::optional<int> hours = saved[i]->FindInt(kArchiveAfterKey);
        hours && *hours >= 0) {
      space->SetArchiveAfter(base::Hours(*hours));
    }
    DeserializeSpace(*saved[i], *space);
  }
  return saved.size();
}

bool IsRestorableURL(const GURL& url) {
  if (!url.is_valid() || url.IsAboutBlank()) {
    return false;
  }
  // chrome://newtab/ and the WebUI page it commits as.
  return !url.SchemeIs("chrome") ||
         (url.host() != "newtab" && url.host() != "new-tab-page");
}

}  // namespace compass
