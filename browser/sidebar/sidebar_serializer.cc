// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/sidebar/sidebar_serializer.h"

#include <optional>
#include <string>
#include <vector>

#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "strata/browser/sidebar/space_model.h"
#include "url/gurl.h"

namespace strata {

namespace {

constexpr int kVersion = 1;

constexpr char kVersionKey[] = "version";
constexpr char kPinnedKey[] = "pinned";
constexpr char kTodayKey[] = "today";
constexpr char kTypeKey[] = "type";
constexpr char kTitleKey[] = "title";
constexpr char kUrlKey[] = "url";
constexpr char kChildrenKey[] = "children";

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
  data.Set(kVersionKey, kVersion);
  data.Set(kPinnedKey, SerializeSection(*model.pinned_root(), false));
  data.Set(kTodayKey, SerializeSection(*model.today_root(), true));
  return data;
}

size_t DeserializeSpace(const base::DictValue& data, SpaceModel& model) {
  if (data.FindInt(kVersionKey) != kVersion) {
    return 0;
  }
  return DeserializeSection(data, kPinnedKey, model, model.pinned_root()) +
         DeserializeSection(data, kTodayKey, model, model.today_root());
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
