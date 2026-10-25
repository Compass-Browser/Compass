// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_WEBUI_SIDEBAR_SIDEBAR_PAGE_HANDLER_H_
#define STRATA_BROWSER_UI_WEBUI_SIDEBAR_SIDEBAR_PAGE_HANDLER_H_

#include <memory>
#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "strata/browser/window_sync/live_tab_registry.h"
#include "strata/browser/window_sync/space_directory.h"
#include "strata/browser/window_sync/window_features.h"
#include "ui/menus/simple_menu_model.h"

class BrowserWindowInterface;
class TabStripModel;

namespace content {
class WebContents;
}  // namespace content

namespace strata {

class SidebarContextMenu;
class SidebarHoverCard;
class TabStripBridge;

// Serves the sidebar page its window's SpaceModel and acts on the page's
// requests. Sends the tree once, then the model's changes as they happen.
// Slice b of docs/design/sidebar.md.
class SidebarPageHandler : public sidebar::mojom::PageHandler,
                           public SpaceModel::Observer,
                           public SpaceList::Observer,
                           public SpaceDirectory::Observer,
                           public WindowFeatures::Observer,
                           public LiveTabRegistry::Observer,
                           public TabStripModelObserver {
 public:
  // What the entries' context menus offer (docs/design/sidebar.md, "Context
  // menus").
  enum MenuCommand {
    kCopyLink = 1,
    kDuplicate,
    kReload,
    kPin,
    kUnpin,
    kAddToFavorites,
    kNewFolder,
    kRename,
    kMoveToSpace,
    kCloseTab,
    kRemove,
    // Then one per space in the "Move to Space" submenu, in their order.
    kMoveToSpaceFirst = 100,
  };

  // |page_contents| is the sidebar page's, for its hover card and context
  // menus (null in tests).
  SidebarPageHandler(
      mojo::PendingReceiver<sidebar::mojom::PageHandler> receiver,
      mojo::PendingRemote<sidebar::mojom::Page> page,
      BrowserWindowInterface* browser,
      content::WebContents* page_contents = nullptr);
  SidebarPageHandler(const SidebarPageHandler&) = delete;
  SidebarPageHandler& operator=(const SidebarPageHandler&) = delete;
  ~SidebarPageHandler() override;

  // sidebar::mojom::PageHandler:
  void RequestTree() override;
  void ActivateNode(const std::string& id) override;
  void CloseNode(const std::string& id) override;
  void NewTab() override;
  void MoveNode(const std::string& id,
                const std::string& new_parent_id,
                uint32_t index) override;
  void OpenLink(const GURL& url,
                const std::string& parent_id,
                uint32_t index) override;
  void AddFolder(const std::string& parent_id,
                 uint32_t index,
                 const std::string& title) override;
  void RenameFolder(const std::string& id, const std::string& title) override;
  void DeleteNode(const std::string& id) override;
  void SwitchSpace(const std::string& id) override;
  void AddSpace() override;
  void RenameSpace(const std::string& id, const std::string& name) override;
  void SetSpaceColor(const std::string& id,
                     sidebar::mojom::SpaceColor color) override;
  void DeleteSpace(const std::string& id) override;
  void MoveNodeToSpace(const std::string& id,
                       const std::string& space_id) override;
  void GetSpaceTree(const std::string& id,
                    GetSpaceTreeCallback callback) override;
  void PreviewSpace(const std::optional<std::string>& id,
                    double progress) override;
  void SetArchiveAfter(const std::string& space_id, uint32_t hours) override;
  void RestoreArchived(const std::string& id) override;
  void RemoveArchived(const std::string& id) override;
  void ClearArchive() override;
  void ShowHoverCard(const std::string& id,
                     int32_t row_top,
                     int32_t row_height) override;
  void HideHoverCard() override;
  void ShowContextMenu(const std::string& id, int32_t x, int32_t y) override;

  // Node |id|'s context menu, or null if it has none. Its commands act on the
  // node, if it's still there, while this handler lives.
  std::unique_ptr<ui::SimpleMenuModel> CreateContextMenu(const std::string& id);
  // Carries out |command|, other than a move to a space, on node |id|.
  void ExecuteMenuCommand(const std::string& id, int command);

  // SpaceModel::Observer:
  void OnNodeAdded(const SidebarNode& node) override;
  void OnNodeMoved(const SidebarNode& node,
                   const SidebarNode& old_parent,
                   size_t old_index) override;
  void OnNodeChanged(const SidebarNode& node) override;
  void OnNodeWillBeRemoved(const SidebarNode& node) override;
  void OnNodeWillMoveToSpace(const SidebarNode& node) override;
  void OnArchiveChanged(const SpaceModel& space) override;

  // SpaceList::Observer:
  void OnSpaceAdded(SpaceModel& space) override;
  void OnSpaceUpdated(const SpaceModel& space) override;
  void OnSpaceWillBeRemoved(SpaceModel& space) override;

  // SpaceDirectory::Observer:
  void OnDirectoryChanged() override;

  // WindowFeatures::Observer:
  void OnSpaceSwitched() override;
  void OnSwappedAway() override;

  // LiveTabRegistry::Observer:
  void OnNodeLiveChanged(const SidebarNode& node, bool live) override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;

 private:
  TabStripModel* tab_strip_model();
  SpaceModel& space_model();
  // The profile's, shown in every window.
  SpaceModel& favorites();
  // The favorites if they hold |node|, else the window's space.
  SpaceModel& GetModelOf(const SidebarNode* node);
  TabStripBridge& bridge();
  LiveTabRegistry& registry();

  sidebar::mojom::NodePtr ToMojo(const SidebarNode& node);
  // With |registry|'s live tabs: another profile's, for its spaces.
  sidebar::mojom::NodePtr ToMojo(const SidebarNode& node,
                                 const LiveTabRegistry& registry);
  void SendSpaces();
  void SendArchive();

  // The node with |id| in the window's space or the favorites, or null if
  // there is none (the page can be behind).
  SidebarNode* FindNode(const std::string& id);
  // The window's profile's space with |id|, or null.
  SpaceModel* FindSpace(const std::string& id);
  // The space with |id| of any profile the switcher lists, or an empty entry.
  SpaceDirectory::Entry FindAnySpace(const std::string& id);
  // Null if the page lists only the profile's spaces.
  SpaceDirectory* directory();

  // The node of the window's active tab, as an ID for the page.
  std::optional<std::string> GetActiveNodeId();

  // Closes |tab_node|'s live tab through the strip, or removes the node if it
  // has none.
  void CloseTabNode(SidebarNode* tab_node);

  // Opens a copy of tab node |node| in the foreground, as a Today entry.
  void DuplicateNode(SidebarNode* node);

  // The page's hover card, or null without one (in tests).
  SidebarHoverCard* hover_card();
  // Runs the page's context menus, or null without one (in tests).
  SidebarContextMenu* context_menu();

  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<WindowFeatures> features_;
  const raw_ptr<content::WebContents> page_contents_;
  // The space whose changes the page gets: the window's, except while it
  // switches.
  raw_ptr<SpaceModel> observed_space_ = nullptr;
  // Whether the page lists every profile's spaces: not in off-the-record
  // windows.
  bool uses_directory_ = false;
  mojo::Receiver<sidebar::mojom::PageHandler> receiver_;
  mojo::Remote<sidebar::mojom::Page> page_;

  base::WeakPtrFactory<SidebarPageHandler> weak_factory_{this};
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_WEBUI_SIDEBAR_SIDEBAR_PAGE_HANDLER_H_
