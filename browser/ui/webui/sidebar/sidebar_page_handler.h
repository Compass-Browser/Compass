// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SIDEBAR_SIDEBAR_PAGE_HANDLER_H_
#define COMPASS_BROWSER_UI_WEBUI_SIDEBAR_SIDEBAR_PAGE_HANDLER_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_attributes_storage_observer.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "compass/browser/claude/claude_activity.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "compass/browser/window_sync/live_tab_registry.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/window_features.h"
#include "components/send_tab_to_self/target_device_info.h"
#include "ui/menus/simple_menu_model.h"

class BrowserWindowInterface;
class GURL;
class TabStripModel;

namespace send_tab_to_self {
class SendTabToSelfModel;
}  // namespace send_tab_to_self

namespace content {
class WebContents;
}  // namespace content

namespace compass {

class ClosedEntries;
class NowPlayingTracker;
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
                           public ProfileAttributesStorageObserver,
                           public WindowFeatures::Observer,
                           public LiveTabRegistry::Observer,
                           public claude::ClaudeActivity::Observer,
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
    kOpenInSplit,
    kSeparate,
    kMute,
    kUnmute,
    kSendToDevices,
    // A pinned tab or favorite that left its saved URL: make its page the
    // saved URL, or go back to it (docs/design/sidebar.md, "Pinned URL").
    kSetSavedUrl,
    kBackToSavedUrl,
    // Then one per space in the "Move to Space" submenu, in their order.
    kMoveToSpaceFirst = 100,
    // Then one per device in "Send to Your Devices", in their order.
    kSendToDeviceFirst = 200,
  };

  // At most this many devices in "Send to Your Devices" (as Chromium's page
  // menu).
  static constexpr size_t kMaxSendTargets = 5;

  // Makes "Send to Your Devices" use |model|, as if sync offered it, while
  // set (tests). Null restores the profile's.
  static void SetSendTabModelForTesting(
      send_tab_to_self::SendTabToSelfModel* model);

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

  BrowserWindowInterface* browser() const { return browser_; }

  // The page's pipes, for the handler of the window the page moves to (a
  // switch to another profile's space keeps the page). Calls the page made
  // since the switch started wait in them.
  std::pair<mojo::PendingReceiver<sidebar::mojom::PageHandler>,
            mojo::PendingRemote<sidebar::mojom::Page>>
  TakePipes();

  // Run whenever the page has taken a tree.
  void set_tree_shown_callback(base::RepeatingClosure callback) {
    tree_shown_callback_ = std::move(callback);
  }

  // sidebar::mojom::PageHandler:
  void RequestTree() override;
  void TreeShown() override;
  void ActivateNode(const std::string& id) override;
  void CloseNode(const std::string& id) override;
  void ClearToday() override;
  void NewTab() override;
  void SetNodeMuted(const std::string& id, bool muted) override;
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
  void RenameTab(const std::string& id, const std::string& title) override;
  void SplitNode(const std::string& id,
                 const std::string& target_id,
                 bool first) override;
  void SetFolderCollapsed(const std::string& id, bool collapsed) override;
  void DeleteNode(const std::string& id) override;
  void SwitchSpace(const std::string& id) override;
  void AddSpace() override;
  void RenameSpace(const std::string& id, const std::string& name) override;
  void SetSpaceColor(const std::string& id,
                     sidebar::mojom::SpaceColorPtr color) override;
  void SetSpaceGradient(
      const std::string& id,
      std::vector<sidebar::mojom::SpaceColorPtr> gradient) override;
  void SetSpaceIntensity(const std::string& id, double intensity) override;
  void SetSpaceGrain(const std::string& id, double grain) override;
  void SetSpaceAppearance(const std::string& id,
                          sidebar::mojom::SpaceAppearance appearance) override;
  void SetSpaceIcon(const std::string& id,
                    sidebar::mojom::SpaceIconPtr icon) override;
  void SetSpaceProfile(const std::string& id,
                       const std::optional<std::string>& profile_id) override;
  void DeleteSpace(const std::string& id) override;
  void MoveSpace(const std::string& id, uint32_t index) override;
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
  void ActOnNowPlaying(const std::string& id,
                       sidebar::mojom::NowPlayingAction action) override;

  // Tells the page about a swipe across the sidebar (CompassSidebarView).
  void SendSwipe(sidebar::mojom::SwipePhase phase, float delta_x);

  // Node |id|'s context menu, or null if it has none. Its commands act on the
  // node, if it's still there, while this handler lives.
  std::unique_ptr<ui::SimpleMenuModel> CreateContextMenu(const std::string& id);
  // Carries out |command|, other than a move to a space or a send, on node
  // |id|.
  void ExecuteMenuCommand(const std::string& id, int command);
  // Sends node |id|'s page to |device| (send tab to self,
  // docs/design/send-tab.md).
  void SendNodeToDevice(const std::string& id,
                        const send_tab_to_self::TargetDeviceInfo& device);

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
  void OnSpaceMoved(const SpaceModel& space) override;

  // SpaceDirectory::Observer:
  void OnDirectoryChanged() override;

  // ProfileAttributesStorageObserver, for the profiles a space can be
  // assigned to:
  void OnProfileAdded(const base::FilePath& profile_path) override;
  void OnProfileWasRemoved(const base::FilePath& profile_path,
                           const std::u16string& profile_name) override;
  void OnProfileNameChanged(const base::FilePath& profile_path,
                            const std::u16string& old_profile_name) override;
  void OnProfileIsOmittedChanged(const base::FilePath& profile_path) override;

  // WindowFeatures::Observer:
  void OnSpaceSwitched() override;
  void OnSwappedAway() override;

  // LiveTabRegistry::Observer:
  void OnNodeLiveChanged(const SidebarNode& node, bool live) override;
  void OnNodeAudioChanged(const SidebarNode& node) override;

  // claude::ClaudeActivity::Observer:
  void OnClaudeActingChanged(content::WebContents* contents) override;

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
  // The profile's, for Cmd+Z.
  ClosedEntries& closed_entries();

  // Stops following the models and the window, for good: on destruction, or
  // once the window swapped away.
  void StopObserving();

  sidebar::mojom::NodePtr ToMojo(const SidebarNode& node);
  // With |registry|'s live tabs: another profile's, for its spaces.
  sidebar::mojom::NodePtr ToMojo(const SidebarNode& node,
                                 const LiveTabRegistry& registry);
  void SendSpaces();
  void SendArchive();
  // The devices |url| can be sent to, most recently active first, or none
  // if sync doesn't offer sending it.
  std::vector<send_tab_to_self::TargetDeviceInfo> GetSendTargets(
      const GURL& url);
  send_tab_to_self::SendTabToSelfModel* send_tab_model();
  void SendNowPlaying();

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

  // Closes |tab_node|'s live tab through the strip of the window that has it,
  // or removes the node if it has none. Recorded for Cmd+Z, unless
  // |recorded|: the caller recorded the step it belongs to.
  void CloseTabNode(SidebarNode* tab_node, bool recorded = false);

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
  // switches. Null once the handler stopped observing.
  raw_ptr<SpaceModel> observed_space_ = nullptr;
  // Whether the page lists every profile's spaces: not in off-the-record
  // windows.
  bool uses_directory_ = false;
  base::ScopedObservation<ProfileAttributesStorage,
                          ProfileAttributesStorageObserver>
      profiles_observation_{this};
  base::RepeatingClosure tree_shown_callback_;
  // The window's now playing widgets. Only with a page: unit tests have no
  // media session service.
  std::unique_ptr<NowPlayingTracker> now_playing_;
  mojo::Receiver<sidebar::mojom::PageHandler> receiver_;
  mojo::Remote<sidebar::mojom::Page> page_;

  base::WeakPtrFactory<SidebarPageHandler> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SIDEBAR_SIDEBAR_PAGE_HANDLER_H_
