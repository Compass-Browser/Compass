// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_SIDEBAR_SIDEBAR_NODE_H_
#define STRATA_BROWSER_SIDEBAR_SIDEBAR_NODE_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/uuid.h"
#include "url/gurl.h"

namespace strata {

// One node in a space's sidebar tree. Nodes are created and changed only
// through SpaceModel, which enforces the tree's invariants.
class SidebarNode {
 public:
  enum class Type {
    // The pinned and today sections. Each space has exactly one of each.
    kRoot,
    kFolder,
    kTab,
    // One sidebar entry that shows its child tabs (panes) side by side.
    kSplit,
  };

  SidebarNode(Type type, std::u16string title, GURL url);
  SidebarNode(const SidebarNode&) = delete;
  SidebarNode& operator=(const SidebarNode&) = delete;
  ~SidebarNode();

  const base::Uuid& id() const { return id_; }
  Type type() const { return type_; }
  const std::u16string& title() const { return title_; }
  const GURL& url() const { return url_; }
  SidebarNode* parent() const { return parent_.get(); }
  const std::vector<std::unique_ptr<SidebarNode>>& children() const {
    return children_;
  }

  // Returns the position of |child|, which must be a child of this node.
  size_t GetIndexOf(const SidebarNode* child) const;

  // Whether |node| is this node or one of its descendants.
  bool Contains(const SidebarNode* node) const;

 private:
  friend class SpaceModel;

  SidebarNode* Add(std::unique_ptr<SidebarNode> child, size_t index);
  std::unique_ptr<SidebarNode> Remove(const SidebarNode* child);

  const base::Uuid id_;
  const Type type_;
  std::u16string title_;
  GURL url_;
  raw_ptr<SidebarNode> parent_ = nullptr;
  std::vector<std::unique_ptr<SidebarNode>> children_;
};

}  // namespace strata

#endif  // STRATA_BROWSER_SIDEBAR_SIDEBAR_NODE_H_
