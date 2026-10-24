// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/sidebar/sidebar_serializer.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "url/gurl.h"

namespace strata {

namespace {

constexpr int kVersion = 2;
constexpr int kSingleSpaceVersion = 1;

constexpr char kVersionKey[] = "version";
constexpr char kActiveKey[] = "active";
constexpr char kSpacesKey[] = "spaces";
constexpr char kIdKey[] = "id";
constexpr char kNameKey[] = "name";
constexpr char kColorKey[] = "color";
constexpr char kPinnedKey[] = "pinned";
constexpr char kTodayKey[] = "today";
constexpr char kTypeKey[] = "type";
constexpr char kTitleKey[] = "title";
constexpr char kUrlKey[] = "url";
constexpr char kChildrenKey[] = "children";

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
  switch (node.type()) {
    case SidebarNode::Type::kTab:
      if (today && !IsRestorableURL(node.url())) {
        return std::nullopt;
      }
      dict.Set(kTypeKey, kTab);
      dict.Set(kUrlKey, node.url().spec());
      break;
    case SidebarNode::Type::kFolder:
      dict.Set(kTypeKey, kFolder);
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
    const std::string* url = dict.FindString(kUrlKey);
    model.AddTab(parent, index, GURL(url ? *url : std::string()),
                 std::move(title16));
    return true;
  }

  const base::ListValue* children = dict.FindList(kChildrenKey);
  if (*type == kFolder) {
    if (parent->type() == SidebarNode::Type::kSplit ||
        parent == model.today_root()) {
      return false;
    }
    SidebarNode* folder = model.AddFolder(parent, index, std::move(title16));
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
    // SpaceModel builds splits from tabs that already exist, so add the panes
    // first, then join them. A split with fewer than two panes is stored as
    // its pane.
    if (parent->type() == SidebarNode::Type::kSplit || !children) {
      return false;
    }
    std::vector<SidebarNode*> panes;
    for (const base::Value& child : *children) {
      const base::DictValue* pane = child.GetIfDict();
      const std::string* pane_type =
          pane ? pane->FindString(kTypeKey) : nullptr;
      if (!pane_type || *pane_type != kTab || panes.size() == kMaxSplitPanes) {
        continue;
      }
      const std::string* url = pane->FindString(kUrlKey);
      const std::string* pane_title = pane->FindString(kTitleKey);
      panes.push_back(model.AddTab(
          parent, index + panes.size(), GURL(url ? *url : std::string()),
          pane_title ? base::UTF8ToUTF16(*pane_title) : u""));
    }
    if (panes.empty()) {
      return false;
    }
    for (size_t i = 1; i < panes.size(); ++i) {
      model.Split(panes[0], panes[i]);
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

}  // namespace

base::DictValue SerializeSpace(const SpaceModel& model) {
  base::DictValue data;
  data.Set(kIdKey, model.id().AsLowercaseString());
  data.Set(kNameKey, base::UTF16ToUTF8(model.name()));
  data.Set(kColorKey, kColorNames[static_cast<size_t>(model.color())]);
  data.Set(kPinnedKey, SerializeSection(*model.pinned_root(), false));
  data.Set(kTodayKey, SerializeSection(*model.today_root(), true));
  return data;
}

size_t DeserializeSpace(const base::DictValue& data, SpaceModel& model) {
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

}  // namespace strata
