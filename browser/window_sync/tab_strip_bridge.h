// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_WINDOW_SYNC_TAB_STRIP_BRIDGE_H_
#define STRATA_BROWSER_WINDOW_SYNC_TAB_STRIP_BRIDGE_H_

#include <vector>

#include "base/containers/flat_map.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/uuid.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/tabs/public/tab_interface.h"
#include "strata/browser/sidebar/space_model.h"

class BrowserWindowInterface;
class TabStripModel;

namespace strata {

class LiveTabRegistry;

// The placeholder a window shows for a tab that moved to another window.
inline constexpr char kPlaceholderHost[] = "strata-placeholder";
inline constexpr char kPlaceholderURL[] = "chrome://strata-placeholder/";

// Keeps a window's TabStripModel in line with its profile's SpaceModel. The
// model is the source of truth: the strip holds the nodes live in this
// window, in the tree's depth-first order. Tabs the strip gains, loses or
// reorders on its own (links, keyboard shortcuts, extensions) are mirrored
// into the Today section. A pinned node outlives its tab: when the tab
// closes, the node stays, unloaded. A tab moving to another window keeps its
// node. When a tab leaves for another window, this window falls back to its
// previous tab, or shows a placeholder for it. See docs/design/sidebar.md,
// slices b to d and "Window sync".
class TabStripBridge : public TabStripModelObserver,
                       public SpaceModel::Observer {
 public:
  TabStripBridge(BrowserWindowInterface* browser,
                 SpaceModel* space_model,
                 LiveTabRegistry* registry);
  TabStripBridge(const TabStripBridge&) = delete;
  TabStripBridge& operator=(const TabStripBridge&) = delete;
  ~TabStripBridge() override;

  // The live tab of |node| in this window, or null.
  tabs::TabInterface* GetTab(const SidebarNode* node) const;

  // The node of |tab|, or null if it has none.
  SidebarNode* GetNode(const tabs::TabInterface* tab) const;

  // Returns |tab_node|'s tab in this window. A tab in another window moves
  // here (move on focus); an unloaded node gets a new tab. Either way it lands
  // in the foreground, at the node's place in the strip.
  tabs::TabInterface* OpenNode(SidebarNode* tab_node);

  // The node a placeholder tab stands for, or null if |tab| isn't one (or its
  // node is gone).
  SidebarNode* GetPlaceholderNode(const tabs::TabInterface* tab) const;

  // The node |tab| shows: its own, or its placeholder's.
  SidebarNode* GetShownNode(const tabs::TabInterface* tab) const;

  // Called before |tab| leaves this window. If it's the active tab, activates
  // the previously used one, or a placeholder if there's none.
  void PrepareToLoseTab(tabs::TabInterface* tab);

  // Moves this window's tabs to |target|, in the background, and closes its
  // placeholders. The strip ends up empty.
  void HandOffTabsTo(TabStripBridge& target);

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab,
                      TabChangeType change_type) override;
  void OnTabStripModelDestroyed(TabStripModel* tab_strip_model) override;

  // SpaceModel::Observer:
  void OnNodeMoved(const SidebarNode& node,
                   const SidebarNode& old_parent,
                   size_t old_index) override;
  void OnNodeWillBeRemoved(const SidebarNode& node) override;

 private:
  // Adds a Today node for |tab|, which the strip just inserted at |index|.
  void AddNodeForTab(tabs::TabInterface* tab, int index);

  // Mirrors a strip move of |tab| to |to_index| into the model, or puts the
  // strip back if the tab's node can't move that way.
  void MoveNodeForTab(tabs::TabInterface* tab, int to_index);

  // Removes |tab|'s node, or only unloads it if it's pinned. A tab that
  // isn't deleted (it moves to another window) keeps its node.
  void RemoveNodeForTab(tabs::TabInterface* tab, bool will_delete);

  bool IsPinned(const SidebarNode* node) const;

  // Copies |tab|'s title and committed URL to its node.
  void UpdateNode(tabs::TabInterface* tab);

  // The Today entry (a direct child of Today) that holds the live tab at
  // strip |index|, or null if there is none.
  SidebarNode* GetTodayEntryAt(int index) const;

  // Where a Today entry goes so that it follows the strip tab at |index| - 1.
  // |moving| is the entry being moved, if any; the result is counted after it
  // has left Today, as SpaceModel::Move() expects.
  size_t GetTodayIndexAfter(int index, const SidebarNode* moving) const;

  // Reorders the strip to the tree's depth-first order. The strip can't be
  // changed while it notifies, so changes it starts are fixed up in a task.
  void SyncStripOrder();
  void ScheduleSyncStripOrder();

  bool IsPlaceholder(const tabs::TabInterface* tab) const;
  void InsertPlaceholder(SidebarNode* node);
  // Closes placeholders that aren't active, or whose node is gone.
  void ClosePlaceholders();
  void ScheduleClosePlaceholders();

  // Moves |tab| to the front of the most-recently-used list.
  void MarkUsed(tabs::TabInterface* tab);

  const raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<TabStripModel> tab_strip_model_;
  const raw_ptr<SpaceModel> space_model_;
  const raw_ptr<LiveTabRegistry> registry_;

  // Set while the bridge changes either side, so it ignores its own echoes.
  bool updating_ = false;

  // Set while OpenNode() inserts a tab: that tab belongs to this node.
  raw_ptr<SidebarNode> adopting_node_ = nullptr;
  // Set while InsertPlaceholder() inserts a placeholder for this node.
  raw_ptr<SidebarNode> placeholder_node_ = nullptr;

  // This window's placeholder tabs, and the nodes they stand for.
  base::flat_map<tabs::TabHandle, base::Uuid> placeholders_;

  // This window's tabs, most recently used first. May hold stale handles.
  std::vector<tabs::TabHandle> mru_;

  base::WeakPtrFactory<TabStripBridge> weak_factory_{this};
};

}  // namespace strata

#endif  // STRATA_BROWSER_WINDOW_SYNC_TAB_STRIP_BRIDGE_H_
