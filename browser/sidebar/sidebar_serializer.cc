// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/sidebar/sidebar_serializer.h"

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
constexpr char kTypeKey[] = "type";
constexpr char kTitleKey[] = "title";
constexpr char kUrlKey[] = "url";
constexpr char kChildrenKey[] = "children";

constexpr char kTab[] = "tab";
constexpr char kFolder[] = "folder";
constexpr char kSplit[] = "split";

base::DictValue SerializeNode(const SidebarNode& node) {
  base::DictValue dict;
  switch (node.type()) {
    case SidebarNode::Type::kTab:
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
  if (!node.children().empty()) {
    base::ListValue children;
    for (const auto& child : node.children()) {
      children.Append(SerializeNode(*child));
    }
    dict.Set(kChildrenKey, std::move(children));
  }
  return dict;
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
    if (parent->type() == SidebarNode::Type::kSplit) {
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

}  // namespace

base::DictValue SerializePinned(const SpaceModel& model) {
  base::ListValue pinned;
  for (const auto& node : model.pinned_root()->children()) {
    pinned.Append(SerializeNode(*node));
  }
  base::DictValue data;
  data.Set(kVersionKey, kVersion);
  data.Set(kPinnedKey, std::move(pinned));
  return data;
}

size_t DeserializePinned(const base::DictValue& data,
                         SpaceModel& model,
                         size_t index) {
  const base::ListValue* pinned = data.FindList(kPinnedKey);
  if (data.FindInt(kVersionKey) != kVersion || !pinned) {
    return 0;
  }
  size_t inserted = 0;
  for (const base::Value& entry : *pinned) {
    if (entry.is_dict() &&
        DeserializeNode(entry.GetDict(), model, model.pinned_root(),
                        index + inserted)) {
      ++inserted;
    }
  }
  return inserted;
}

}  // namespace strata
