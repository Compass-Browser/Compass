// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SIDEBAR_SIDEBAR_NODE_H_
#define COMPASS_BROWSER_SIDEBAR_SIDEBAR_NODE_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "url/gurl.h"

namespace compass {

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

  // |id| is for nodes that keep an identity they had before (saved, or from
  // another device); an invalid one gets a fresh ID.
  SidebarNode(Type type,
              std::u16string title,
              GURL url,
              const base::Uuid& id = base::Uuid());
  SidebarNode(const SidebarNode&) = delete;
  SidebarNode& operator=(const SidebarNode&) = delete;
  ~SidebarNode();

  // Stable: saved with the node, and the same on every synced device.
  const base::Uuid& id() const { return id_; }
  Type type() const { return type_; }
  // A tab's page title, a folder's name.
  const std::u16string& title() const { return title_; }
  // The name the user gave a tab, or empty. Kept while its page's title
  // changes, and saved.
  const std::u16string& custom_title() const { return custom_title_; }
  // What the sidebar shows: the custom title, else the title.
  const std::u16string& display_title() const {
    return custom_title_.empty() ? title_ : custom_title_;
  }
  // The page a tab shows: follows its live tab's navigations.
  const GURL& url() const { return url_; }
  // A pinned tab's or favorite's own page, which its tab goes back to when
  // it closes (Arc's "pinned URL"), and its title. Set when the tab is
  // pinned or made a favorite, and saved; empty elsewhere
  // (docs/design/sidebar.md, "Pinned URL").
  const GURL& saved_url() const { return saved_url_; }
  const std::u16string& saved_title() const { return saved_title_; }
  // When a tab node was last shown in a window, or null if that's unknown
  // (see SpaceModel::ArchiveIdleEntries()). Unset for other nodes.
  base::Time last_active() const { return last_active_; }
  // Whether a folder shows only its own row. Shared by every window, and
  // saved. False for other nodes.
  bool collapsed() const { return collapsed_; }
  // Whether a tab is muted: its live tab is, and it will be when it next
  // opens. Follows the live tab's mute state, and saved. False for other
  // nodes (docs/design/sidebar.md, "Sound").
  bool muted() const { return muted_; }
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
  std::u16string custom_title_;
  GURL url_;
  GURL saved_url_;
  std::u16string saved_title_;
  base::Time last_active_;
  bool collapsed_ = false;
  bool muted_ = false;
  raw_ptr<SidebarNode> parent_ = nullptr;
  std::vector<std::unique_ptr<SidebarNode>> children_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_SIDEBAR_SIDEBAR_NODE_H_
