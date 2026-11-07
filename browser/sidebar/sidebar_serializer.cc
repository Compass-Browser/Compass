// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/sidebar_serializer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json/values_util.h"
#include "base/notreached.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
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
constexpr char kIconKey[] = "icon";
constexpr char kGradientKey[] = "gradient";
constexpr char kIntensityKey[] = "intensity";
constexpr char kGrainKey[] = "grain";
constexpr char kAppearanceKey[] = "appearance";
constexpr char kEmojiKey[] = "emoji";
constexpr char kSymbolKey[] = "symbol";
constexpr char kPinnedKey[] = "pinned";
constexpr char kTodayKey[] = "today";
constexpr char kTypeKey[] = "type";
constexpr char kTitleKey[] = "title";
constexpr char kUrlKey[] = "url";
constexpr char kChildrenKey[] = "children";
constexpr char kLastActiveKey[] = "last_active";
constexpr char kCollapsedKey[] = "collapsed";
constexpr char kCustomTitleKey[] = "custom_title";
constexpr char kMutedKey[] = "muted";
constexpr char kSavedUrlKey[] = "saved_url";
constexpr char kSavedTitleKey[] = "saved_title";
constexpr char kArchiveAfterKey[] = "archive_after_hours";
constexpr char kArchiveKey[] = "archive";
constexpr char kArchivedAtKey[] = "archived";
constexpr char kTabsKey[] = "tabs";

// A palette color by its name (older versions read only those), any other as
// "#rrggbb".
std::string SerializeColor(SpaceColor color) {
  for (const NamedSpaceColor& named : kSpaceColorPalette) {
    if (named.color == color) {
      return std::string(named.name);
    }
  }
  const SkColor tint = *color.tint();
  return base::StringPrintf("#%02x%02x%02x", SkColorGetR(tint),
                            SkColorGetG(tint), SkColorGetB(tint));
}

// {"emoji": "..."} or {"symbol": "..."}; none isn't saved.
base::DictValue SerializeIcon(const SpaceIcon& icon) {
  base::DictValue dict;
  dict.Set(icon.kind() == SpaceIcon::Kind::kEmoji ? kEmojiKey : kSymbolKey,
           icon.value());
  return dict;
}

std::optional<SpaceIcon> DeserializeIcon(const base::DictValue& saved) {
  if (const std::string* emoji = saved.FindString(kEmojiKey)) {
    return SpaceIcon::Emoji(*emoji);
  }
  if (const std::string* symbol = saved.FindString(kSymbolKey)) {
    return SpaceIcon::Symbol(*symbol);
  }
  return std::nullopt;
}

std::optional<SpaceColor> DeserializeColor(std::string_view saved) {
  for (const NamedSpaceColor& named : kSpaceColorPalette) {
    if (named.name == saved) {
      return named.color;
    }
  }
  uint32_t rgb;
  if (saved.size() != 7 || saved[0] != '#' ||
      !std::ranges::all_of(saved.substr(1), base::IsHexDigit<char>) ||
      !base::HexStringToUInt(saved.substr(1), &rgb)) {
    return std::nullopt;
  }
  return SpaceColor::FromSkColor(rgb);
}

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
      if (node.muted()) {
        dict.Set(kMutedKey, true);
      }
      if (node.saved_url().is_valid()) {
        dict.Set(kSavedUrlKey, node.saved_url().spec());
        dict.Set(kSavedTitleKey, base::UTF16ToUTF8(node.saved_title()));
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
  model.SetMuted(tab, dict.FindBool(kMutedKey).value_or(false));
  // A file from before saved URLs gave the tab its page as it closed (set
  // by AddTab()).
  const std::string* saved_url = dict.FindString(kSavedUrlKey);
  if (saved_url && GURL(*saved_url).is_valid() && !tab->saved_url().is_empty()) {
    const std::string* saved_title = dict.FindString(kSavedTitleKey);
    model.SetSavedUrl(tab, GURL(*saved_url),
                      saved_title ? base::UTF8ToUTF16(*saved_title)
                                  : std::u16string());
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
      base::DictValue tab_dict;
      tab_dict.Set(kTitleKey, base::UTF16ToUTF8(tab.title));
      tab_dict.Set(kUrlKey, tab.url.spec());
      if (tab.id.is_valid()) {
        tab_dict.Set(kIdKey, tab.id.AsLowercaseString());
      }
      tabs.Append(std::move(tab_dict));
    }
    entries.Append(base::DictValue()
                       .Set(kIdKey, entry.id.AsLowercaseString())
                       .Set(kArchivedAtKey, base::TimeToValue(entry.archived_at))
                       .Set(kTabsKey, std::move(tabs)));
  }
  return entries;
}

// The "id" of an archived entry or tab, or an invalid one.
base::Uuid ArchivedId(const base::DictValue& dict) {
  const std::string* id = dict.FindString(kIdKey);
  return id ? base::Uuid::ParseLowercase(*id) : base::Uuid();
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
    // Older files have no IDs: the entry keeps the fresh one it has, and a
    // single tab shares it (see ArchivedEntry::id).
    const base::Uuid saved_id = ArchivedId(*dict);
    if (saved_id.is_valid() && !model.FindArchived(saved_id)) {
      entry.id = saved_id;
    }
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
      // Without one (an older file), a lone tab shares the entry's.
      base::Uuid tab_id = ArchivedId(*tab_dict);
      if (!tab_id.is_valid()) {
        tab_id = tabs->size() == 1 ? entry.id : base::Uuid::GenerateRandomV4();
      }
      entry.tabs.push_back(
          {title ? base::UTF8ToUTF16(*title) : u"", GURL(*url), tab_id});
    }
    model.AddArchived(std::move(entry));
  }
}

}  // namespace

base::DictValue SerializeSpace(const SpaceModel& model) {
  base::DictValue data;
  data.Set(kIdKey, model.id().AsLowercaseString());
  data.Set(kNameKey, base::UTF16ToUTF8(model.name()));
  data.Set(kColorKey, SerializeColor(model.color()));
  if (!model.gradient().empty()) {
    base::ListValue gradient;
    for (SpaceColor color : model.gradient()) {
      gradient.Append(SerializeColor(color));
    }
    data.Set(kGradientKey, std::move(gradient));
  }
  // Left out when they're the defaults.
  if (model.intensity() != kDefaultSpaceIntensity) {
    data.Set(kIntensityKey, model.intensity());
  }
  if (model.grain() > 0) {
    data.Set(kGrainKey, model.grain());
  }
  if (model.appearance() != SpaceAppearance::kAuto) {
    data.Set(kAppearanceKey, model.appearance() == SpaceAppearance::kLight
                                 ? "light"
                                 : "dark");
  }
  if (!model.icon().is_none()) {
    data.Set(kIconKey, SerializeIcon(model.icon()));
  }
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
    if (const std::string* saved_color = saved[i]->FindString(kColorKey)) {
      if (std::optional<SpaceColor> color = DeserializeColor(*saved_color)) {
        space->SetColor(*color);
      }
    }
    // After the color, which a gradient needs. Unreadable colors are left out.
    if (const base::ListValue* saved_gradient =
            saved[i]->FindList(kGradientKey)) {
      std::vector<SpaceColor> gradient;
      for (const base::Value& entry : *saved_gradient) {
        if (const std::string* saved_color = entry.GetIfString()) {
          if (std::optional<SpaceColor> color =
                  DeserializeColor(*saved_color)) {
            gradient.push_back(*color);
          }
        }
      }
      space->SetGradient(std::move(gradient));
    }
    if (std::optional<double> intensity = saved[i]->FindDouble(kIntensityKey)) {
      space->SetIntensity(*intensity);
    }
    if (std::optional<double> grain = saved[i]->FindDouble(kGrainKey)) {
      space->SetGrain(*grain);
    }
    // Anything else (from a later version) is auto.
    if (const std::string* appearance = saved[i]->FindString(kAppearanceKey)) {
      space->SetAppearance(*appearance == "light" ? SpaceAppearance::kLight
                           : *appearance == "dark" ? SpaceAppearance::kDark
                                                   : SpaceAppearance::kAuto);
    }
    // Unreadable icons (from a later version) are left out.
    if (const base::DictValue* saved_icon = saved[i]->FindDict(kIconKey)) {
      if (std::optional<SpaceIcon> icon = DeserializeIcon(*saved_icon)) {
        space->SetIcon(std::move(*icon));
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

base::DictValue SerializeEntry(const SidebarNode& entry) {
  std::optional<base::DictValue> dict = SerializeNode(entry, /*today=*/false);
  return dict ? std::move(*dict) : base::DictValue();
}

SidebarNode* DeserializeEntry(const base::DictValue& data,
                              SpaceModel& model,
                              SidebarNode* parent,
                              size_t index) {
  index = std::min(index, parent->children().size());
  if (!DeserializeNode(data, model, parent, index)) {
    return nullptr;
  }
  return parent->children()[index].get();
}

bool IsRestorableURL(const GURL& url) {
  if (!url.is_valid() || url.IsAboutBlank()) {
    return false;
  }
  // chrome://newtab/, the WebUI page it committed as, and the empty page
  // that replaced it (window_sync/empty_tab.h).
  return !url.SchemeIs("chrome") ||
         (url.host() != "newtab" && url.host() != "new-tab-page" &&
          url.host() != "compass-empty");
}

}  // namespace compass
