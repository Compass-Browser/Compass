// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_SIDEBAR_SPACE_MODEL_H_
#define STRATA_BROWSER_SIDEBAR_SPACE_MODEL_H_

#include <cstddef>
#include <memory>
#include <string>

#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "strata/browser/sidebar/sidebar_node.h"

namespace strata {

// Matches upstream split tabs (MultiContentsView). See ARCHITECTURE.md §9.
inline constexpr size_t kMaxSplitPanes = 2;

// The sidebar tree of one space: a pinned section (folders, tabs and splits;
// folders nest) and a today section (tabs and splits). Every window shows the
// same SpaceModel. The model knows nothing about WebContents or windows.
//
// Invariants:
//  - Folders only live in the pinned section.
//  - Splits contain only tabs, at most kMaxSplitPanes of them.
//  - A split left with one pane is replaced by that pane, in its position.
//  - The two roots can't be moved or removed.
class SpaceModel {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnNodeAdded(const SidebarNode& node) {}
    virtual void OnNodeMoved(const SidebarNode& node,
                             const SidebarNode& old_parent,
                             size_t old_index) {}
    virtual void OnNodeChanged(const SidebarNode& node) {}
    // Called before |node| and its subtree are destroyed.
    virtual void OnNodeWillBeRemoved(const SidebarNode& node) {}
  };

  SpaceModel();
  SpaceModel(const SpaceModel&) = delete;
  SpaceModel& operator=(const SpaceModel&) = delete;
  ~SpaceModel();

  SidebarNode* pinned_root() { return pinned_root_.get(); }
  const SidebarNode* pinned_root() const { return pinned_root_.get(); }
  SidebarNode* today_root() { return today_root_.get(); }
  const SidebarNode* today_root() const { return today_root_.get(); }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  SidebarNode* AddFolder(SidebarNode* parent,
                         size_t index,
                         std::u16string title);
  SidebarNode* AddTab(SidebarNode* parent,
                      size_t index,
                      const GURL& url,
                      std::u16string title);

  // Whether |node| may be moved into |new_parent|. Use it to validate drops.
  bool CanMove(const SidebarNode* node, const SidebarNode* new_parent) const;

  // Moves |node| into |new_parent| at |index|, counted after |node| has left
  // its current parent.
  void Move(SidebarNode* node, SidebarNode* new_parent, size_t index);

  // Whether |tab| can be shown side by side with |target|.
  bool CanSplit(const SidebarNode* target, const SidebarNode* tab) const;

  // Shows |tab| next to |target|. If |target| is a standalone tab, both
  // become panes of a new split in |target|'s position. If |target| is a split
  // or one of its panes, |tab| becomes the split's last pane. Returns the
  // split.
  SidebarNode* Split(SidebarNode* target, SidebarNode* tab);

  // Removes |node| and its subtree.
  void Remove(SidebarNode* node);

  void SetTitle(SidebarNode* node, std::u16string title);
  void SetUrl(SidebarNode* tab, const GURL& url);

  SidebarNode* FindById(const base::Uuid& id);

 private:
  bool CanContain(const SidebarNode* parent, SidebarNode::Type type) const;

  // Restores the split invariants after |split| lost a pane.
  void NormalizeSplit(SidebarNode* split);

  SidebarNode* AddNode(SidebarNode* parent,
                       size_t index,
                       std::unique_ptr<SidebarNode> node);

  std::unique_ptr<SidebarNode> pinned_root_;
  std::unique_ptr<SidebarNode> today_root_;
  base::ObserverList<Observer> observers_;
};

}  // namespace strata

#endif  // STRATA_BROWSER_SIDEBAR_SPACE_MODEL_H_
