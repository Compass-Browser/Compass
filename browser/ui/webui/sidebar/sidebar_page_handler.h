// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_WEBUI_SIDEBAR_SIDEBAR_PAGE_HANDLER_H_
#define STRATA_BROWSER_UI_WEBUI_SIDEBAR_SIDEBAR_PAGE_HANDLER_H_

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
#include "strata/browser/window_sync/window_features.h"

class BrowserWindowInterface;
class TabStripModel;

namespace content {
class WebContents;
}  // namespace content

namespace strata {

class SidebarHoverCard;
class TabStripBridge;

// Serves the sidebar page its window's SpaceModel and acts on the page's
// requests. Sends the tree once, then the model's changes as they happen.
// Slice b of docs/design/sidebar.md.
class SidebarPageHandler : public sidebar::mojom::PageHandler,
                           public SpaceModel::Observer,
                           public SpaceList::Observer,
                           public WindowFeatures::Observer,
                           public LiveTabRegistry::Observer,
                           public TabStripModelObserver {
 public:
  // |page_contents| is the sidebar page's, for its hover card (null in
  // tests).
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
  void SetArchiveAfter(const std::string& space_id, uint32_t hours) override;
  void RestoreArchived(const std::string& id) override;
  void RemoveArchived(const std::string& id) override;
  void ClearArchive() override;
  void ShowHoverCard(const std::string& id,
                     int32_t row_top,
                     int32_t row_height) override;
  void HideHoverCard() override;

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

  // WindowFeatures::Observer:
  void OnSpaceSwitched() override;

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
  TabStripBridge& bridge();
  LiveTabRegistry& registry();

  sidebar::mojom::NodePtr ToMojo(const SidebarNode& node);
  void SendSpaces();
  void SendArchive();

  // The node with |id|, or null if there is none (the page can be behind).
  SidebarNode* FindNode(const std::string& id);
  // The space with |id|, or null.
  SpaceModel* FindSpace(const std::string& id);

  // The node of the window's active tab, as an ID for the page.
  std::optional<std::string> GetActiveNodeId();

  // Closes |tab_node|'s live tab through the strip, or removes the node if it
  // has none.
  void CloseTabNode(SidebarNode* tab_node);

  // The page's hover card, or null without one (in tests).
  SidebarHoverCard* hover_card();

  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<WindowFeatures> features_;
  const raw_ptr<content::WebContents> page_contents_;
  // The space whose changes the page gets: the window's, except while it
  // switches.
  raw_ptr<SpaceModel> observed_space_ = nullptr;
  mojo::Receiver<sidebar::mojom::PageHandler> receiver_;
  mojo::Remote<sidebar::mojom::Page> page_;

  base::WeakPtrFactory<SidebarPageHandler> weak_factory_{this};
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_WEBUI_SIDEBAR_SIDEBAR_PAGE_HANDLER_H_
