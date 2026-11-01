// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_WINDOW_SYNC_TAB_STRIP_BRIDGE_H_
#define COMPASS_BROWSER_WINDOW_SYNC_TAB_STRIP_BRIDGE_H_

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/containers/flat_map.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/uuid.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/tabs/public/tab_interface.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/live_tab_registry.h"
#include "url/gurl.h"

class BrowserWindowInterface;
class TabStripModel;

namespace compass {

class RestoredTabMatcher;

// The placeholder a window shows for a tab that moved to another window.
inline constexpr char kPlaceholderHost[] = "compass-placeholder";
// chrome://compass-import: importing from Arc (docs/design/arc-import.md).
inline constexpr char kArcImportHost[] = "compass-import";
// chrome://compass-settings: Compass's own settings, the shortcuts for now
// (docs/design/shortcuts.md "Customizing").
inline constexpr char kSettingsHost[] = "compass-settings";
inline constexpr char kPlaceholderURL[] = "chrome://compass-placeholder/";

// Keeps a window's TabStripModel in line with its space's SpaceModel and the
// profile's favorites. The models are the source of truth: the strip holds the
// nodes live in this window, favorites first, then the space's tree depth
// first. Tabs the strip gains, loses or reorders on its own (links,
// keyboard shortcuts, extensions) are mirrored into the Today section. A pinned
// node (or a favorite) outlives its tab: when the tab closes, the node stays,
// unloaded. So does a Today node whose tab closes with its window. A tab moving
// to another window keeps its node. A tab Chrome restores takes over its saved
// entry. When the tab a window shows leaves for another window, the window
// keeps showing it: a placeholder with a frozen screenshot takes its place. See
// docs/design/sidebar.md, slices b to d and "Window sync".
class TabStripBridge : public TabStripModelObserver,
                       public SpaceModel::Observer,
                       public LiveTabRegistry::Observer {
 public:
  TabStripBridge(BrowserWindowInterface* browser,
                 SpaceModel* space_model,
                 SpaceModel* favorites,
                 LiveTabRegistry* registry,
                 RestoredTabMatcher* restored_tab_matcher,
                 bool mirrors_tabs = true);
  TabStripBridge(const TabStripBridge&) = delete;
  TabStripBridge& operator=(const TabStripBridge&) = delete;
  ~TabStripBridge() override;

  // The live tab of |node| in this window, or null.
  tabs::TabInterface* GetTab(const SidebarNode* node) const;

  // The node of |tab|, or null if it has none.
  SidebarNode* GetNode(const tabs::TabInterface* tab) const;

  // Pinned or a favorite: the node outlives its tab.
  bool IsPinned(const SidebarNode* node) const;

  // Returns |tab_node|'s tab in this window. A tab in another window moves
  // here (move on focus); an unloaded node gets a new tab. Either way it lands
  // in the foreground, at the node's place in the strip. A pane brings the
  // rest of its split, in the background, so the split shows.
  tabs::TabInterface* OpenNode(SidebarNode* tab_node);

  // The node a placeholder tab stands for, or null if |tab| isn't one (or its
  // node is gone).
  SidebarNode* GetPlaceholderNode(const tabs::TabInterface* tab) const;

  // Runs |callback| with a data: URL of the screenshot |placeholder| shows,
  // once it's ready, or with an empty string if there's none.
  void GetSnapshot(const tabs::TabInterface* placeholder,
                   base::OnceCallback<void(const std::string&)> callback);

  // The node |tab| shows: its own, or its placeholder's.
  SidebarNode* GetShownNode(const tabs::TabInterface* tab) const;

  // Called before |tab| leaves this window. If it's the active tab, a
  // placeholder with its screenshot takes its place, so the window keeps
  // showing it. Moves that skip this (upstream commands, extensions) get the
  // same, a moment after the tab has gone.
  void PrepareToLoseTab(tabs::TabInterface* tab);

  // Moves this window's tabs to |target|, in the background, and closes its
  // placeholders. The strip ends up empty.
  void HandOffTabsTo(TabStripBridge& target);

  // The strip just emptied (see compass::KeepWindowWithNoTabs()). Returns
  // true if the window stays: its last tab was closed, not moved away, and
  // wasn't an empty tab. An empty tab then opens, in a task. Asking again
  // before then gives the same answer.
  bool KeepWindowWithNoTabs();

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab,
                      TabChangeType change_type) override;
  void OnTabStripModelDestroyed(TabStripModel* tab_strip_model) override;
  void OnSplitTabChanged(const SplitTabChange& change) override;
  void TabGroupedStateChanged(TabStripModel* tab_strip_model,
                              std::optional<tab_groups::TabGroupId> old_group,
                              std::optional<tab_groups::TabGroupId> new_group,
                              tabs::TabInterface* tab,
                              int index) override;
  void OnTabGroupChanged(const TabGroupChange& change) override;

  // SpaceModel::Observer:
  void OnNodeAdded(const SidebarNode& node) override;
  void OnNodeMoved(const SidebarNode& node,
                   const SidebarNode& old_parent,
                   size_t old_index) override;
  void OnNodeWillBeRemoved(const SidebarNode& node) override;
  void OnNodeWillMoveToSpace(const SidebarNode& node) override;

  // LiveTabRegistry::Observer:
  void OnNodeLiveChanged(const SidebarNode& node, bool live) override;

 private:
  // Adds a Today node for |tab|, which the strip just inserted at |index|.
  void AddNodeForTab(tabs::TabInterface* tab, int index);

  // OpenNode() for one tab node, in the foreground or not.
  tabs::TabInterface* OpenTabNode(SidebarNode* tab_node, bool foreground);

  // Splits (docs/design/sidebar.md, "Split tabs"). The model's splits whose
  // panes are both live here are upstream splits too, and the other way
  // round. The model split of |a| and |b|'s nodes, or null.
  SidebarNode* GetModelSplit(const tabs::TabInterface* a,
                             const tabs::TabInterface* b) const;
  // Upstream split |a| and |b| (in strip order): the model follows.
  void MirrorUpstreamSplit(tabs::TabInterface* a, tabs::TabInterface* b);
  // An upstream split of these tabs ended. If both are still here, the user
  // separated them, and the model follows. Pinned panes that closed keep
  // their split.
  void OnUpstreamSplitRemoved(std::vector<tabs::TabHandle> tabs);
  // Tab groups (docs/design/sidebar.md, "Tab groups"): each upstream group
  // becomes a Today folder, and the group ends.
  void ScheduleConvertGroups();
  void ConvertGroups();

  // Ends the upstream splits that aren't model splits.
  void RemoveUnmirroredSplits();
  // Makes upstream splits of the model splits live here.
  void CreateMirroredSplits();

  // Where the node of |tab|, just inserted at |index|, goes if it's a link
  // opened in the background (Cmd+click, middle click) from a page in the
  // sidebar: with its opener, in a Today folder, made if needed. Returns
  // false to place it as usual. See docs/design/sidebar.md, "Link folders".
  bool GetLinkFolderPlace(tabs::TabInterface* tab,
                          int index,
                          SidebarNode*& folder,
                          size_t& folder_index);

  // Mirrors a strip move of |tab| to |to_index| into the model, or puts the
  // strip back if the tab's node can't move that way.
  void MoveNodeForTab(tabs::TabInterface* tab, int to_index);

  // Removes |tab|'s node, or only unloads it if it's pinned or its window is
  // closing. A tab that isn't deleted (it moves to another window) keeps its
  // node.
  void RemoveNodeForTab(tabs::TabInterface* tab, bool will_delete);

  // The roots whose live tabs make up the strip, in strip order.
  std::array<const SidebarNode*, 3> GetRoots() const;
  // The node |id| in the space or the favorites, or null.
  SidebarNode* FindNode(const base::Uuid& id) const;

  // Copies |tab|'s title and committed URL to its node, when they changed
  // in the tab. A node whose URL or title sync changed (another device
  // navigated it) keeps them until its tab here navigates or retitles, so the
  // two devices don't take the entry back and forth (docs/design/sync.md,
  // "Slice 3b notes").
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

  // A frozen screenshot of a tab that left, shown by its placeholder.
  struct Snapshot {
    Snapshot();
    Snapshot(Snapshot&&);
    Snapshot& operator=(Snapshot&&);
    ~Snapshot();

    // Unset while capturing and encoding; empty if that failed.
    std::optional<std::string> data_url;
    std::vector<base::OnceCallback<void(const std::string&)>> waiters;
  };

  // Starts capturing |tab|, which is about to leave, for |node_id|'s
  // placeholder.
  void CaptureSnapshot(tabs::TabInterface* tab, const base::Uuid& node_id);
  void OnSnapshotReady(const base::Uuid& node_id, std::string data_url);
  void DropSnapshot(const base::Uuid& node_id);

  // When the window becomes active on a placeholder, its tab comes back.
  void OnWindowBecameActive(BrowserWindowInterface* browser);
  void BringBackShownPlaceholder();

  bool IsPlaceholder(const tabs::TabInterface* tab) const;
  // Inserts and activates a placeholder for |node| at strip |index|.
  void InsertPlaceholder(SidebarNode* node, int index);
  // For a tab that already left: unless the window is going away or the node
  // is back.
  void InsertPlaceholderAfterMove(const base::Uuid& node_id, int index);
  // Closes placeholders that aren't active, or whose node is gone.
  void ClosePlaceholders();
  void ScheduleClosePlaceholders();

  // Empty tabs (empty_tab.h) last only while they're shown.
  void CloseHiddenEmptyTabs();
  void OpenEmptyTabIfStillEmpty();

  const raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<TabStripModel> tab_strip_model_;
  const raw_ptr<SpaceModel> space_model_;
  // The profile's, mirrored ahead of the space.
  const raw_ptr<SpaceModel> favorites_;
  const raw_ptr<LiveTabRegistry> registry_;
  const raw_ptr<RestoredTabMatcher> restored_tab_matcher_;

  // False for a window outside the sidebar (a little window): the bridge
  // then neither mirrors its strip nor follows the model.
  const bool mirrors_tabs_;

  // Set while the bridge changes either side, so it ignores its own echoes.
  bool updating_ = false;
  // Set while HandOffTabsTo() empties this closing window.
  bool handing_off_ = false;
  // Whether the strip's last removal closed a tab the user had open, rather
  // than moving it away or closing an empty tab.
  bool last_removal_closed_tab_ = false;
  // Set from KeepWindowWithNoTabs() until the empty tab opens.
  bool keeping_window_ = false;

  // Set while OpenNode() inserts a tab: that tab belongs to this node.
  raw_ptr<SidebarNode> adopting_node_ = nullptr;
  // Set while InsertPlaceholder() inserts a placeholder for this node.
  raw_ptr<SidebarNode> placeholder_node_ = nullptr;

  // Today folders made for links opened from pinned entries and favorites,
  // by the opener's node.
  base::flat_map<base::Uuid, base::Uuid> link_folders_;

  // Set while a ConvertGroups() task is pending.
  bool convert_groups_scheduled_ = false;

  // This window's placeholder tabs, and the nodes they stand for.
  base::flat_map<tabs::TabHandle, base::Uuid> placeholders_;

  // The URL and title UpdateNode() last saw in each tab.
  struct SeenContent {
    GURL url;
    std::u16string title;
  };
  base::flat_map<tabs::TabHandle, SeenContent> seen_;

  // Screenshots for this window's placeholders, by node.
  base::flat_map<base::Uuid, Snapshot> snapshots_;

  base::CallbackListSubscription did_become_active_subscription_;

  base::WeakPtrFactory<TabStripBridge> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_WINDOW_SYNC_TAB_STRIP_BRIDGE_H_
