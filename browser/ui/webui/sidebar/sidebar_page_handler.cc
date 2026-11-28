// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sidebar/sidebar_page_handler.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/fixed_flat_map.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/uuid.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/sync/send_tab_to_self_sync_service_factory.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/send_tab_to_self/send_tab_to_self_util.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_muted_utils.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "compass/browser/claude/ai_provider.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/claude/claude_quick.h"
#include "compass/browser/dev_mode/dev_mode.h"
#include "compass/browser/prefs/copy_link.h"
#include "compass/browser/rooms/room_model.h"
#include "compass/browser/rooms/room_service.h"
#include "compass/browser/sidebar/closed_entries.h"
#include "compass/browser/sync/compass_sync_service.h"
#include "compass/browser/sync/compass_sync_service_factory.h"
#include "compass/browser/ui/easels/easel_capture.h"
#include "compass/browser/ui/live_folders/live_plugin_registry.h"
#include "compass/browser/ui/views/archive_bubble/archive_bubble.h"
#include "compass/browser/ui/views/claude/claude_panel.h"
#include "compass/browser/ui/views/command_bar/command_bar.h"
#include "compass/browser/ui/views/little_window/little_window.h"
#include "compass/browser/ui/views/live_folder_editor_bubble/live_folder_editor_bubble.h"
#include "compass/browser/ui/views/share_bubble/share_bubble.h"
#include "compass/browser/ui/views/sidebar/sidebar_context_menu.h"
#include "compass/browser/ui/views/sidebar/sidebar_hover_card.h"
#include "compass/browser/ui/views/space_editor_bubble/space_editor_bubble.h"
#include "compass/browser/ui/webui/sidebar/now_playing_tracker.h"
#include "compass/browser/ui/webui/sidebar/space_mojo.h"
#include "compass/browser/ui/webui/sidebar/update_notice.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/undo_close.h"
#include "compass/browser/window_sync/window_features.h"
#include "compass/strings/grit/compass_strings.h"
#include "components/prefs/pref_service.h"
#include "components/send_tab_to_self/entry_point_display_reason.h"
#include "components/send_tab_to_self/metrics_util.h"
#include "components/send_tab_to_self/page_context.h"
#include "components/send_tab_to_self/send_tab_to_self_entry.h"
#include "components/send_tab_to_self/send_tab_to_self_model.h"
#include "components/send_tab_to_self/send_tab_to_self_sync_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/menu_separator_types.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/base/window_open_disposition_utils.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"

namespace compass {

namespace {

// The share bubble opened from a row's menu points at the row: this far
// above and below where the menu was opened (a row is 32 high).
constexpr int kShareAnchorHalfHeight = 16;

sidebar::mojom::LiveFolderPtr ToMojoLiveFolder(const SidebarNode& node) {
  if (!node.is_live_folder()) {
    return nullptr;
  }
  auto folder = sidebar::mojom::LiveFolder::New();
  folder->plugin_id = node.live_source()->plugin_id;
  if (const LivePlugin* plugin =
          LivePluginRegistry::Get().Find(folder->plugin_id)) {
    folder->plugin_name = plugin->manifest.name;
    folder->icon_url = plugin->IconUrl();
  }
  return folder;
}

sidebar::mojom::LiveFolderStatePtr ToMojoLiveState(
    const LiveFolderState* state) {
  auto result = sidebar::mojom::LiveFolderState::New();
  if (state) {
    result->refreshing = state->refreshing;
    result->error = state->error ? state->error->message : std::string();
  }
  return result;
}

void CollectLiveFolderIds(const SidebarNode& node,
                          std::vector<base::Uuid>& ids) {
  if (node.is_live_folder()) {
    ids.push_back(node.id());
    return;
  }
  for (const auto& child : node.children()) {
    CollectLiveFolderIds(*child, ids);
  }
}

sidebar::mojom::AudioState ToMojoAudio(LiveTabRegistry::AudioState audio) {
  switch (audio) {
    case LiveTabRegistry::AudioState::kNone:
      return sidebar::mojom::AudioState::kNone;
    case LiveTabRegistry::AudioState::kAudible:
      return sidebar::mojom::AudioState::kAudible;
    case LiveTabRegistry::AudioState::kMuted:
      return sidebar::mojom::AudioState::kMuted;
  }
  NOTREACHED();
}

sidebar::mojom::NodeType ToMojoType(SidebarNode::Type type) {
  switch (type) {
    case SidebarNode::Type::kRoot:
      return sidebar::mojom::NodeType::kRoot;
    case SidebarNode::Type::kFolder:
      return sidebar::mojom::NodeType::kFolder;
    case SidebarNode::Type::kTab:
      return sidebar::mojom::NodeType::kTab;
    case SidebarNode::Type::kSplit:
      return sidebar::mojom::NodeType::kSplit;
  }
}

send_tab_to_self::SendTabToSelfModel* g_send_tab_model_for_testing = nullptr;

// A node's context menu. Commands go to the handler by the node's ID, so a
// node that's gone by then is left alone.
class NodeMenu : public ui::SimpleMenuModel,
                 public ui::SimpleMenuModel::Delegate {
 public:
  NodeMenu(base::WeakPtr<SidebarPageHandler> handler, std::string id)
      : ui::SimpleMenuModel(this),
        handler_(std::move(handler)),
        id_(std::move(id)),
        spaces_menu_(this),
        devices_menu_(this) {}
  NodeMenu(const NodeMenu&) = delete;
  NodeMenu& operator=(const NodeMenu&) = delete;
  ~NodeMenu() override = default;

  // Adds "Move to Space" with |spaces| other than the window's, if there are
  // any.
  void AddMoveToSpace(const std::vector<std::unique_ptr<SpaceModel>>& spaces,
                      const SpaceModel& current) {
    for (const auto& space : spaces) {
      if (space.get() == &current) {
        continue;
      }
      spaces_menu_.AddItem(
          SidebarPageHandler::kMoveToSpaceFirst + space_ids_.size(),
          space->name());
      space_ids_.push_back(space->id().AsLowercaseString());
    }
    if (!space_ids_.empty()) {
      AddSubMenu(
          SidebarPageHandler::kMoveToSpace,
          l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_MOVE_TO_SPACE),
          &spaces_menu_);
    }
  }

  // Adds "Send to Your Devices" with |devices|, if there are any.
  void AddSendToDevices(
      std::vector<syncer::RemoteDeviceInfo> devices) {
    devices_ = std::move(devices);
    for (size_t i = 0; i < devices_.size(); ++i) {
      devices_menu_.AddItem(SidebarPageHandler::kSendToDeviceFirst + i,
                            base::UTF8ToUTF16(devices_[i].device_name));
    }
    if (!devices_.empty()) {
      AddSubMenu(
          SidebarPageHandler::kSendToDevices,
          l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_SEND_TO_DEVICES),
          &devices_menu_);
    }
  }

  // ui::SimpleMenuModel::Delegate:
  void ExecuteCommand(int command_id, int event_flags) override {
    if (!handler_) {
      return;
    }
    if (command_id >= SidebarPageHandler::kSendToDeviceFirst) {
      const size_t index = command_id - SidebarPageHandler::kSendToDeviceFirst;
      if (index < devices_.size()) {
        handler_->SendNodeToDevice(id_, devices_[index]);
      }
      return;
    }
    if (command_id >= SidebarPageHandler::kMoveToSpaceFirst) {
      handler_->MoveNodeToSpace(
          id_, space_ids_.at(command_id - SidebarPageHandler::kMoveToSpaceFirst));
      return;
    }
    handler_->ExecuteMenuCommand(id_, command_id);
  }

 private:
  const base::WeakPtr<SidebarPageHandler> handler_;
  const std::string id_;
  ui::SimpleMenuModel spaces_menu_;
  std::vector<std::string> space_ids_;
  ui::SimpleMenuModel devices_menu_;
  std::vector<syncer::RemoteDeviceInfo> devices_;
};

}  // namespace

SidebarPageHandler::SidebarPageHandler(
    mojo::PendingReceiver<sidebar::mojom::PageHandler> receiver,
    mojo::PendingRemote<sidebar::mojom::Page> page,
    BrowserWindowInterface* browser,
    content::WebContents* page_contents)
    : browser_(browser),
      features_(WindowFeatures::From(browser)),
      page_contents_(page_contents),
      receiver_(this, std::move(receiver)),
      page_(std::move(page)) {
  CHECK(features_);
  observed_space_ = &space_model();
  observed_space_->AddObserver(this);
  favorites().AddObserver(this);
  features_->service().space_list().AddObserver(this);
  if (SpaceDirectory::IsListable(browser_->GetProfile())) {
    if (SpaceDirectory* directory = SpaceDirectory::Get()) {
      uses_directory_ = true;
      directory->AddObserver(this);
      profiles_observation_.Observe(&g_browser_process->profile_manager()
                                         ->GetProfileAttributesStorage());
    }
  }
  features_->AddObserver(this);
  registry().AddObserver(this);
  claude::ClaudeActivity::AddObserver(this);
  tab_strip_model()->AddObserver(this);
  if (LiveFolderService* live_folders =
          LiveFolderService::Get(browser_->GetProfile())) {
    live_folders_observation_.Observe(live_folders);
    previews_observation_.Observe(&live_folders->previews());
  }
  // Claude's names for link folders, and Tidy Today; incognito windows
  // have neither.
  if (!browser_->GetProfile()->IsOffTheRecord()) {
    if (LinkFolderNamer* namer =
            LinkFolderNamer::Get(browser_->GetProfile())) {
      namer_observation_.Observe(namer);
    }
    // Shorter Tab Titles follows the profile's spaces from now on.
    ShortTitler::Get(browser_->GetProfile());
    // Shared items' badges and view-only state.
    if (rooms::RoomService* rooms =
            rooms::RoomService::Get(browser_->GetProfile())) {
      rooms_observation_.Observe(rooms);
    }
    if (TodayTidier* tidier = TodayTidier::Get(browser_->GetProfile())) {
      tidier_observation_.Observe(tidier);
      tidy_prefs_.Init(browser_->GetProfile()->GetPrefs());
      const base::RepeatingClosure send = base::BindRepeating(
          &SidebarPageHandler::SendTidyState, base::Unretained(this),
          /*force=*/false);
      tidy_prefs_.Add(claude::prefs::kTidyToday, send);
      tidy_prefs_.Add(claude::prefs::kSidebarOnDevice, send);
      claude::prefs::ObserveProviderPrefs(
          tidy_prefs_, {claude::prefs::kApiKey, claude::prefs::kBaseUrl},
          send);
    }
  }
  if (page_contents_) {
    now_playing_ = std::make_unique<NowPlayingTracker>(
        browser_, base::BindRepeating(&SidebarPageHandler::SendNowPlaying,
                                      base::Unretained(this)));
    update_notice_ = std::make_unique<UpdateNotice>(base::BindRepeating(
        &SidebarPageHandler::SendUpdateReady, base::Unretained(this)));
  }
}

SidebarPageHandler::~SidebarPageHandler() {
  // A swipe the page was in the middle of.
  if (observed_space_ && features_->preview_space()) {
    features_->SetSpacePreview(nullptr, 0);
  }
  StopObserving();
}

void SidebarPageHandler::StopObserving() {
  if (!observed_space_) {
    return;
  }
  registry().RemoveObserver(this);
  live_folders_observation_.Reset();
  previews_observation_.Reset();
  namer_observation_.Reset();
  tidier_observation_.Reset();
  tidy_prefs_.RemoveAll();
  claude::ClaudeActivity::RemoveObserver(this);
  features_->RemoveObserver(this);
  if (SpaceDirectory* directory = this->directory()) {
    directory->RemoveObserver(this);
  }
  profiles_observation_.Reset();
  features_->service().space_list().RemoveObserver(this);
  favorites().RemoveObserver(this);
  observed_space_->RemoveObserver(this);
  observed_space_ = nullptr;
  tab_strip_model()->RemoveObserver(this);
  now_playing_.reset();
  update_notice_.reset();
}

void SidebarPageHandler::RequestTree() {
  SendSpaces();
  auto tree = sidebar::mojom::Tree::New();
  tree->favorites = ToMojo(*favorites().pinned_root());
  tree->pinned = ToMojo(*space_model().pinned_root());
  tree->today = ToMojo(*space_model().today_root());
  tree->has_pinned = space_model().has_pinned();
  tree->active_id = GetActiveNodeId();
  page_->SetTree(std::move(tree));
  // Live folders' refreshing and errors.
  std::vector<base::Uuid> live_folders;
  CollectLiveFolderIds(*space_model().pinned_root(), live_folders);
  LiveFolderService* service = LiveFolderService::Get(browser_->GetProfile());
  for (const base::Uuid& id : live_folders) {
    page_->LiveFolderStateChanged(
        id.AsLowercaseString(),
        ToMojoLiveState(service ? service->GetState(id) : nullptr));
  }
  SendNowPlaying();
  SendUpdateReady();
  SendTidyState(/*force=*/true);
}

void SidebarPageHandler::TreeShown() {
  if (tree_shown_callback_) {
    tree_shown_callback_.Run();
  }
}

void SidebarPageHandler::ActivateNode(const std::string& id) {
  SidebarNode* node = FindNode(id);
  if (!node || node->type() != SidebarNode::Type::kTab) {
    return;
  }
  // A live folder's new entry, seen.
  if (node->unseen()) {
    GetModelOf(node).SetUnseen(node, false);
  }
  // Opening an unloaded node activates its new tab.
  if (tabs::TabInterface* tab = bridge().OpenNode(node)) {
    tab_strip_model()->ActivateTabAt(tab_strip_model()->GetIndexOfTab(tab));
  }
}

void SidebarPageHandler::CloseNode(const std::string& id) {
  if (SidebarNode* node = FindNode(id)) {
    CloseSidebarEntry(*features_, node);
  }
}

void SidebarPageHandler::ClearToday() {
  SpaceModel& space = space_model();
  // One step for Cmd+Z, which brings Today back as it was, folders too.
  ClosedEntries::ScopedStep step(closed_entries());
  for (const auto& entry : space.today_root()->children()) {
    closed_entries().RecordRemoval(space, *entry);
  }
  // Closing a tab changes the tree (splits dissolve, folders close), so go
  // by ID.
  std::vector<base::Uuid> tabs;
  std::vector<base::Uuid> folders;
  std::vector<const SidebarNode*> pending = {space.today_root()};
  while (!pending.empty()) {
    const SidebarNode* node = pending.back();
    pending.pop_back();
    for (const auto& child : node->children()) {
      if (child->type() == SidebarNode::Type::kTab) {
        tabs.push_back(child->id());
      } else {
        if (child->type() == SidebarNode::Type::kFolder) {
          folders.push_back(child->id());
        }
        pending.push_back(child.get());
      }
    }
  }
  for (const base::Uuid& tab_id : tabs) {
    if (SidebarNode* tab = space.FindById(tab_id)) {
      CloseTabNode(tab, /*recorded=*/true);
    }
  }
  // Folders that never held a tab, or whose tabs all closed.
  for (const base::Uuid& folder_id : folders) {
    SidebarNode* folder = space.FindById(folder_id);
    if (!folder || !space.today_root()->Contains(folder)) {
      continue;
    }
    bool has_tab = false;
    std::vector<const SidebarNode*> inside = {folder};
    while (!inside.empty() && !has_tab) {
      const SidebarNode* node = inside.back();
      inside.pop_back();
      for (const auto& child : node->children()) {
        has_tab |= child->type() == SidebarNode::Type::kTab;
        inside.push_back(child.get());
      }
    }
    if (!has_tab) {
      space.Remove(folder);
    }
  }
}

void SidebarPageHandler::NewTab() {
  if (CommandBar* bar = CommandBar::From(browser_)) {
    bar->Show(command_bar::mojom::Mode::kNewEntry);
    return;
  }
  // No views (unit tests).
  chrome::NewTab(browser_, NewTabTypes::kNewTabButton);
}

void SidebarPageHandler::SetNodeMuted(const std::string& id, bool muted) {
  SidebarNode* node = FindNode(id);
  if (!node || node->type() != SidebarNode::Type::kTab) {
    return;
  }
  // Live in any window: its speaker shows in every window of the space.
  if (tabs::TabInterface* tab = registry().GetTab(node)) {
    SetTabAudioMuted(tab->GetContents(), muted,
                     TabMutedReason::kAudioIndicator, std::string());
  }
}

void SidebarPageHandler::MoveNode(const std::string& id,
                                  const std::string& new_parent_id,
                                  uint32_t index) {
  SidebarNode* node = FindNode(id);
  SidebarNode* new_parent = FindNode(new_parent_id);
  if (IsViewOnly(node) || IsViewOnly(new_parent)) {
    return;
  }
  if (!node || !new_parent || index > new_parent->children().size()) {
    return;
  }
  SpaceModel& model = GetModelOf(node);
  SpaceModel& target = GetModelOf(new_parent);
  if (&model != &target) {
    // Into or out of the favorites. The node isn't among |new_parent|'s
    // children, so |index| needs no adjusting.
    MoveEntry(browser_->GetProfile(), node, target, new_parent, index);
    return;
  }
  if (!model.CanMove(node, new_parent)) {
    return;
  }
  // SpaceModel::Move() counts the index after the node has left its parent.
  if (node->parent() == new_parent && new_parent->GetIndexOf(node) < index) {
    --index;
  }
  model.Move(node, new_parent, index);
}

void SidebarPageHandler::OpenLink(const GURL& url,
                                  const std::string& parent_id,
                                  uint32_t index) {
  SidebarNode* parent = FindNode(parent_id);
  if (IsViewOnly(parent)) {
    return;
  }
  if (!parent || index > parent->children().size() ||
      (parent->type() != SidebarNode::Type::kRoot &&
       parent->type() != SidebarNode::Type::kFolder) ||
      !(url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile())) {
    return;
  }
  SpaceModel& model = GetModelOf(parent);
  // Not into an incognito space's Pinned, which it hasn't.
  if (!model.CanContain(parent, SidebarNode::Type::kTab)) {
    return;
  }
  // The page's title replaces the URL once it loads.
  SidebarNode* node =
      model.AddTab(parent, index, url, base::UTF8ToUTF16(url.spec()));
  bridge().OpenNode(node);
}

void SidebarPageHandler::AddFolder(const std::string& parent_id,
                                   uint32_t index,
                                   const std::string& title) {
  SidebarNode* parent = FindNode(parent_id);
  if (IsViewOnly(parent)) {
    return;
  }
  if (!parent || index > parent->children().size() ||
      !space_model().Contains(parent) ||
      (parent->type() != SidebarNode::Type::kRoot &&
       parent->type() != SidebarNode::Type::kFolder) ||
      !space_model().CanContain(parent, SidebarNode::Type::kFolder)) {
    return;
  }
  space_model().AddFolder(parent, index, base::UTF8ToUTF16(title));
}

void SidebarPageHandler::NewEasel() {
  easels::NewEaselInWindow(browser_);
}

void SidebarPageHandler::RenameFolder(const std::string& id,
                                      const std::string& title) {
  SidebarNode* node = FindNode(id);
  if (IsViewOnly(node)) {
    return;
  }
  if (!node || node->type() != SidebarNode::Type::kFolder) {
    return;
  }
  // A name the user gives (or keeps editing) is theirs: Claude doesn't name
  // the folder again (docs/design/ai-sidebar.md). Leaving the field as it
  // was changes nothing.
  const std::u16string title16 = base::UTF8ToUTF16(title);
  if (title16 != node->title()) {
    GetModelOf(node).SetFolderName(node, title16,
                                   SidebarNode::NameSource::kUser);
  }
}

void SidebarPageHandler::RenameTab(const std::string& id,
                                   const std::string& title) {
  SidebarNode* node = FindNode(id);
  if (IsViewOnly(node)) {
    return;
  }
  if (!node || node->type() != SidebarNode::Type::kTab) {
    return;
  }
  std::u16string title16;
  base::TrimWhitespace(base::UTF8ToUTF16(title), base::TRIM_ALL, &title16);
  GetModelOf(node).SetCustomTitle(node, std::move(title16));
}

void SidebarPageHandler::SplitNode(const std::string& id,
                                   const std::string& target_id,
                                   bool first) {
  SidebarNode* node = FindNode(id);
  SidebarNode* target = FindNode(target_id);
  if (IsViewOnly(node) || IsViewOnly(target)) {
    return;
  }
  SpaceModel& space = space_model();
  if (!node || !target || !space.Contains(node) || !space.Contains(target) ||
      !space.CanSplit(target, node)) {
    return;
  }
  // On a pane: next to it, on the side it was dropped on.
  const bool on_pane = target->parent()->type() == SidebarNode::Type::kSplit;
  const size_t pane_index =
      on_pane ? target->parent()->GetIndexOf(target) + (first ? 0 : 1) : 0;
  SidebarNode* split = space.Split(target, node);
  if (on_pane) {
    // Split() put it last; counted without it, as Move() wants.
    space.Move(node, split, pane_index);
  } else if (first) {
    space.Move(node, split, 0);
  }
  // Shows the split, its other pane too.
  ActivateNode(id);
}

void SidebarPageHandler::DropNodeOutside(const std::string& id) {
  SidebarNode* node = FindNode(id);
  if (IsViewOnly(node)) {
    return;
  }
  // A pane isn't dragged alone, and a split's tabs would have to leave their
  // upstream split first.
  if (!node || node->type() != SidebarNode::Type::kTab ||
      node->parent()->type() == SidebarNode::Type::kSplit) {
    return;
  }
  // The drag has ended: the pointer is where it was dropped.
  const gfx::Point point = display::Screen::Get()->GetCursorScreenPoint();
  if (!IsOutsideCompassWindows(point)) {
    return;
  }
  // Live, perhaps in another window showing this space: the tab moves, as
  // it is, and its window's bridge lets the node go.
  if (tabs::TabInterface* tab = registry().GetTab(node)) {
    BrowserWindowInterface* source = tab->GetBrowserWindowInterface();
    WindowFeatures* features = source ? WindowFeatures::From(source) : nullptr;
    if (!features) {
      return;
    }
    Profile* profile = source->GetProfile();
    std::unique_ptr<tabs::TabModel> detached =
        features->tab_strip_bridge().ReleaseTab(tab);
    OpenTabInLittleWindowAt(profile, std::move(detached), point);
    return;
  }
  // Unloaded: its page opens there. A Today entry goes; a pinned one or a
  // favorite stays.
  const GURL url = node->url();
  if (!url.is_valid()) {
    return;
  }
  if (space_model().today_root()->Contains(node)) {
    space_model().Remove(node);
  }
  OpenLittleWindowAt(browser_->GetProfile(), url, point);
}

void SidebarPageHandler::SetFolderCollapsed(const std::string& id,
                                            bool collapsed) {
  SidebarNode* node = FindNode(id);
  if (IsViewOnly(node)) {
    return;
  }
  if (node && node->type() == SidebarNode::Type::kFolder) {
    space_model().SetCollapsed(node, collapsed);
  }
}

void SidebarPageHandler::DeleteNode(const std::string& id) {
  SidebarNode* node = FindNode(id);
  if (IsViewOnly(node)) {
    return;
  }
  if (!node || node->type() == SidebarNode::Type::kRoot) {
    return;
  }
  // Today's tabs close instead; a Today folder goes with its tabs.
  if (space_model().pinned_root()->Contains(node) ||
      favorites().Contains(node) ||
      node->type() == SidebarNode::Type::kFolder) {
    SpaceModel& model = GetModelOf(node);
    ClosedEntries::ScopedStep step(closed_entries());
    closed_entries().RecordRemoval(model, *node);
    // Its live tabs close with it, and their history goes with the step.
    ExpectLiveTabsToClose(registry(), *node, closed_entries());
    model.Remove(node);
  }
}

void SidebarPageHandler::SwitchSpace(const std::string& id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  if (entry.space) {
    // Another profile's swaps the window, whose next handler takes the page
    // (CompassSidebarUI).
    SwitchWindowToSpace(browser_, entry.profile, *entry.space);
  }
}

void SidebarPageHandler::GetSpaceTree(const std::string& id,
                                      GetSpaceTreeCallback callback) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  if (!entry.space) {
    std::move(callback).Run(nullptr);
    return;
  }
  // Another profile's space has its own favorites and tabs.
  SpaceService& service = *SpaceService::Get(entry.profile);
  const LiveTabRegistry& registry = service.registry();
  auto tree = sidebar::mojom::Tree::New();
  tree->favorites =
      ToMojo(*service.space_list().favorites().pinned_root(), registry);
  tree->pinned = ToMojo(*entry.space->pinned_root(), registry);
  tree->today = ToMojo(*entry.space->today_root(), registry);
  tree->has_pinned = entry.space->has_pinned();
  // What the switch would show, for the preview to highlight.
  if (SidebarNode* shown = PredictShownEntry(service, *entry.space)) {
    tree->active_id = shown->id().AsLowercaseString();
  }
  std::move(callback).Run(std::move(tree));
}

void SidebarPageHandler::PreviewSpace(const std::optional<std::string>& id,
                                      double progress) {
  features_->SetSpacePreview(id ? FindAnySpace(*id).space.get() : nullptr,
                             progress);
}

void SidebarPageHandler::AddSpace() {
  features_->AddSpaceAndSwitch();
}

void SidebarPageHandler::RenameSpace(const std::string& id,
                                     const std::string& name) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::RenameSpace(*space, name);
  }
}

void SidebarPageHandler::SetSpaceColor(
    const std::string& id,
    sidebar::mojom::SpaceColorPtr color) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceColor(*space, color);
  }
}

void SidebarPageHandler::SetSpaceGradient(
    const std::string& id,
    std::vector<sidebar::mojom::SpaceColorPtr> gradient) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceGradient(*space, gradient);
  }
}

void SidebarPageHandler::SetSpaceIntensity(const std::string& id,
                                           double intensity) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceIntensity(*space, intensity);
  }
}

void SidebarPageHandler::SetSpaceGrain(const std::string& id, double grain) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceGrain(*space, grain);
  }
}

void SidebarPageHandler::SetSpaceTranslucency(const std::string& id,
                                              double translucency) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceTranslucency(*space, translucency);
  }
}

void SidebarPageHandler::SetSpaceAppearance(
    const std::string& id,
    sidebar::mojom::SpaceAppearance appearance) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceAppearance(*space, appearance);
  }
}

void SidebarPageHandler::SetSpaceIcon(const std::string& id,
                                      sidebar::mojom::SpaceIconPtr icon) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceIcon(*space, icon);
  }
}

void SidebarPageHandler::SetSpaceProfile(
    const std::string& id,
    const std::optional<std::string>& profile_id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  if (!entry.space || !directory() || !g_browser_process->profile_manager()) {
    return;
  }
  std::optional<base::FilePath> path;
  if (profile_id) {
    path = ProfilePathFromId(*profile_id);
    if (!path) {
      return;
    }
  }
  AssignSpaceToProfile(entry.space->id(), std::move(path));
}

void SidebarPageHandler::DeleteSpace(const std::string& id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  if (entry.space) {
    compass::DeleteSpace(entry.profile, *entry.space);
  }
}

void SidebarPageHandler::MoveSpace(const std::string& id, uint32_t index) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  if (!entry.space) {
    return;
  }
  if (SpaceDirectory* directory = this->directory()) {
    directory->MoveSpace(*entry.space, index);
  } else {
    features_->service().space_list().MoveSpace(*entry.space, index);
  }
}

void SidebarPageHandler::MoveNodeToSpace(const std::string& id,
                                         const std::string& space_id) {
  SidebarNode* node = FindNode(id);
  if (IsViewOnly(node)) {
    return;
  }
  SpaceModel* space = FindSpace(space_id);
  if (node && space) {
    MoveEntryToSpace(browser_->GetProfile(), node, *space);
  }
}

void SidebarPageHandler::SetArchiveAfter(const std::string& space_id,
                                         uint32_t hours) {
  if (SpaceModel* space = FindAnySpace(space_id).space) {
    SetSpaceArchiveAfter(*space, hours);
  }
}

void SidebarPageHandler::SetSpaceSynced(const std::string& space_id,
                                        bool synced) {
  if (SpaceModel* space = FindAnySpace(space_id).space) {
    space->SetSynced(synced);
  }
}

void SidebarPageHandler::ToggleArchive(int32_t x,
                                       int32_t y,
                                       int32_t width,
                                       int32_t height) {
  if (SidebarContextMenu* menu = context_menu()) {
    ToggleArchiveBubble(browser_,
                        menu->ToScreen(gfx::Rect(x, y, width, height)));
  }
}

void SidebarPageHandler::ShowHoverCard(const std::string& id,
                                       int32_t row_top,
                                       int32_t row_height) {
  SidebarHoverCard* card = hover_card();
  if (!card) {
    return;
  }
  SidebarNode* node = FindNode(id);
  if (!node || node->type() != SidebarNode::Type::kTab || row_height <= 0) {
    card->Hide();
    return;
  }
  tabs::TabInterface* tab = registry().GetTab(node);
  tabs::TabData data;
  if (tab) {
    data = tabs::TabData::FromTabInterface(tab);
  } else {
    data.title = node->title();
    data.visible_url = node->url();
    data.last_committed_url = node->url();
  }
  // The window's active tab is on screen already.
  const bool is_active = tab && tab == tab_strip_model()->GetActiveTab();
  card->Show(data, row_top, row_height, /*show_preview=*/tab && !is_active,
             tab ? tab->GetContents() : nullptr, is_active,
             base::BindRepeating(&SidebarPageHandler::OnHoverPreviewClicked,
                                 weak_factory_.GetWeakPtr(), id));
}

void SidebarPageHandler::OnHoverPreviewClicked(const std::string& id,
                                               const LivePreviewItem& item,
                                               int event_flags) {
  SidebarNode* node = FindNode(id);
  if (!node || node->type() != SidebarNode::Type::kTab) {
    return;
  }
  // Checked by ParseLivePreviewResult(); a plugin's word all the same.
  const GURL& url = item.url.is_valid() && item.url.SchemeIsHTTPOrHTTPS()
                        ? item.url
                        : GURL::EmptyGURL();
  const WindowOpenDisposition disposition =
      ui::DispositionFromEventFlags(event_flags);
  // Cmd (Ctrl) or a middle click on a row with a link: a new tab, as a link
  // on the page would open.
  if (!url.is_empty() && disposition != WindowOpenDisposition::kCurrentTab) {
    NavigateParams params(browser_, url, ui::PAGE_TRANSITION_LINK);
    params.disposition = disposition;
    Navigate(&params);
    return;
  }
  // Otherwise the tab, at the row's link if it has one. An unloaded entry
  // opens there.
  tabs::TabInterface* tab = registry().GetTab(node);
  if (!url.is_empty() && !tab) {
    GetModelOf(node).SetUrl(node, url);
  }
  ActivateNode(id);
  tab = registry().GetTab(node);
  if (!url.is_empty() && tab &&
      tab->GetContents()->GetLastCommittedURL() != url &&
      tab->GetContents()->GetVisibleURL() != url) {
    // The bridge follows the navigation to the node.
    tab->GetContents()->GetController().LoadURL(
        url, content::Referrer(), ui::PAGE_TRANSITION_LINK, std::string());
  }
}

void SidebarPageHandler::PrefetchHoverPreview(const std::string& id) {
  // While the card waits to open: a plugin's preview of the tab
  // (docs/design/live-folders.md, "Tab previews") is ready when it does.
  SidebarNode* node = FindNode(id);
  LiveFolderService* service = LiveFolderService::Get(browser_->GetProfile());
  if (!node || node->type() != SidebarNode::Type::kTab || !service) {
    return;
  }
  tabs::TabInterface* tab = registry().GetTab(node);
  if (tab && tab == tab_strip_model()->GetActiveTab()) {
    return;
  }
  const GURL url = tab ? tab->GetContents()->GetLastCommittedURL() : node->url();
  if (!service->previews().FindPlugin(url)) {
    return;
  }
  service->previews().Fetch(tab ? tab->GetContents() : nullptr, url,
                            base::DoNothing());
}

void SidebarPageHandler::HideHoverCard() {
  if (SidebarHoverCard* card = hover_card()) {
    card->Hide();
  }
}

void SidebarPageHandler::ReleaseHoverCard() {
  if (SidebarHoverCard* card = hover_card()) {
    card->Release();
  }
}

void SidebarPageHandler::ShowContextMenu(const std::string& id,
                                         int32_t x,
                                         int32_t y) {
  SidebarContextMenu* menu = context_menu();
  if (!menu) {
    return;
  }
  if (std::unique_ptr<ui::SimpleMenuModel> model = CreateContextMenu(id)) {
    HideHoverCard();
    menu->Show(std::move(model), gfx::Point(x, y));
  }
}

void SidebarPageHandler::ActOnNowPlaying(
    const std::string& id,
    sidebar::mojom::NowPlayingAction action) {
  if (!now_playing_) {
    return;
  }
  using Mojo = sidebar::mojom::NowPlayingAction;
  using Action = NowPlayingTracker::Action;
  static constexpr auto kActions = base::MakeFixedFlatMap<Mojo, Action>({
      {Mojo::kPlay, Action::kPlay},
      {Mojo::kPause, Action::kPause},
      {Mojo::kPreviousTrack, Action::kPreviousTrack},
      {Mojo::kNextTrack, Action::kNextTrack},
      {Mojo::kPictureInPicture, Action::kPictureInPicture},
      {Mojo::kMute, Action::kMute},
      {Mojo::kUnmute, Action::kUnmute},
      {Mojo::kDismiss, Action::kDismiss},
      {Mojo::kShowTab, Action::kShowTab},
  });
  now_playing_->Act(id, kActions.at(action));
}

void SidebarPageHandler::ActOnUpdateNotice(
    sidebar::mojom::UpdateNoticeAction action) {
  // Only while it shows: the page can be behind.
  if (!update_notice_ || !update_notice_->ShouldShow()) {
    return;
  }
  switch (action) {
    case sidebar::mojom::UpdateNoticeAction::kRelaunch:
      UpdateNotice::Relaunch();
      return;
    case sidebar::mojom::UpdateNoticeAction::kDismiss:
      update_notice_->Dismiss();
      return;
  }
}

std::unique_ptr<ui::SimpleMenuModel> SidebarPageHandler::CreateContextMenu(
    const std::string& id) {
  SidebarNode* node = FindNode(id);
  if (!node || node->type() == SidebarNode::Type::kRoot) {
    return nullptr;
  }
  auto menu = std::make_unique<NodeMenu>(weak_factory_.GetWeakPtr(), id);
  const bool favorite = favorites().Contains(node);
  const bool pinned = space_model().pinned_root()->Contains(node);
  // Pin, or Unpin; neither in incognito, which has no Pinned.
  const bool pin_item = pinned || space_model().has_pinned();
  const SpaceList& space_list = features_->service().space_list();

  if (node->type() == SidebarNode::Type::kFolder) {
    if (node->is_live_folder()) {
      menu->AddItem(kRefreshLiveFolder,
                    l10n_util::GetStringUTF16(
                        IDS_COMPASS_SIDEBAR_MENU_REFRESH_LIVE_FOLDER));
      menu->AddItem(
          kEditLiveSource,
          l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_EDIT_LIVE_SOURCE));
      menu->AddItem(kMarkAllSeen, l10n_util::GetStringUTF16(
                                      IDS_COMPASS_SIDEBAR_MENU_MARK_ALL_SEEN));
      menu->AddItem(kConvertToPlainFolder,
                    l10n_util::GetStringUTF16(
                        IDS_COMPASS_SIDEBAR_MENU_CONVERT_TO_FOLDER));
      menu->AddSeparator(ui::NORMAL_SEPARATOR);
    } else {
      menu->AddItem(kNewFolder, l10n_util::GetStringUTF16(
                                    IDS_COMPASS_SIDEBAR_CONTEXT_NEW_FOLDER));
      if (space_model().CanAddLiveFolder(node) &&
          LiveFolderService::Get(browser_->GetProfile())) {
        menu->AddItem(kNewLiveFolder,
                      l10n_util::GetStringUTF16(
                          IDS_COMPASS_SIDEBAR_MENU_NEW_LIVE_FOLDER));
        // Or one Claude writes a plugin for (docs/design/claude.md,
        // "Plugins").
        if (browser_->GetProfile()->GetPrefs()->GetBoolean(
                claude::prefs::kPlugins)) {
          menu->AddItem(kDescribeLiveFolder,
                        l10n_util::GetStringUTF16(
                            IDS_COMPASS_SIDEBAR_MENU_DESCRIBE_LIVE_FOLDER));
        }
      }
    }
    menu->AddItem(kRename, l10n_util::GetStringUTF16(IDS_COMPASS_RENAME));
    // Claude's names (docs/design/ai-sidebar.md): back to the name before
    // Claude's, or one from Claude for a folder that has none of its.
    if (node->name_source() == SidebarNode::NameSource::kClaude &&
        !node->name_before_claude().empty()) {
      menu->AddItem(kUndoName, l10n_util::GetStringUTF16(
                                   IDS_COMPASS_SIDEBAR_MENU_UNDO_NAME));
    } else if (!node->is_live_folder() && !node->children().empty() &&
               CanSuggestNames()) {
      menu->AddItem(kSuggestName, l10n_util::GetStringUTF16(
                                      IDS_COMPASS_SIDEBAR_MENU_SUGGEST_NAME));
    }
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
    if (pin_item) {
      menu->AddItem(
          pinned ? kUnpin : kPin,
          pinned ? l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_UNPIN)
                 : l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_PIN));
    }
    menu->AddMoveToSpace(space_list.spaces(), space_model());
    // Sharing a pinned folder with other people.
    if (pinned && !node->is_live_folder() && CanShare()) {
      menu->AddSeparator(ui::NORMAL_SEPARATOR);
      menu->AddItem(
          kShare,
          IsShared(node)
              ? l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_SHARING)
              : l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_SHARE));
    }
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
    menu->AddItem(kRemove, l10n_util::GetStringUTF16(
                               IDS_COMPASS_SIDEBAR_MENU_DELETE_FOLDER));
    return menu;
  }

  // A split acts as one entry; its panes only as tabs.
  const bool entry = node->parent()->type() != SidebarNode::Type::kSplit;
  bool live = registry().GetTab(node) != nullptr;
  for (const auto& pane : node->children()) {
    live |= registry().GetTab(pane.get()) != nullptr;
  }
  // Shown next to the window's active entry, or separated.
  SidebarNode* shown = bridge().GetShownNode(tab_strip_model()->GetActiveTab());
  if (node->type() == SidebarNode::Type::kSplit ||
      node->parent()->type() == SidebarNode::Type::kSplit) {
    menu->AddItem(kSeparate, l10n_util::GetStringUTF16(
                                 IDS_COMPASS_SIDEBAR_MENU_SEPARATE_TABS));
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
  } else if (entry && !favorite && shown && shown != node &&
             space_model().Contains(shown) &&
             space_model().CanSplit(shown, node)) {
    menu->AddItem(kOpenInSplit, l10n_util::GetStringUTF16(
                                    IDS_COMPASS_SIDEBAR_MENU_OPEN_IN_SPLIT));
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
  }
  if (node->type() == SidebarNode::Type::kTab) {
    // A favorite's tile shows no name.
    if (!favorite) {
      menu->AddItem(kRename, l10n_util::GetStringUTF16(IDS_COMPASS_RENAME));
    }
    menu->AddItem(kCopyLink, l10n_util::GetStringUTF16(
                                 IDS_COMPASS_SIDEBAR_MENU_COPY_LINK));
    // An easel's tab: share the easel.
    if (!rooms::EaselOfTab(*node).empty() && CanShare()) {
      menu->AddItem(kShare, l10n_util::GetStringUTF16(
                                IDS_COMPASS_SIDEBAR_MENU_SHARE_EASEL));
    }
    if (entry) {
      menu->AddItem(kDuplicate, l10n_util::GetStringUTF16(
                                    IDS_COMPASS_SIDEBAR_MENU_DUPLICATE));
    }
    menu->AddSendToDevices(GetSendTargets(node->url()));
    // The node hears of its tab's navigations up to 200 ms late.
    if (tabs::TabInterface* tab = registry().GetTab(node)) {
      bridge().UpdateNode(tab);
    }
    if (!node->saved_url().is_empty() && node->url().is_valid() &&
        node->url() != node->saved_url()) {
      menu->AddItem(kSetSavedUrl,
                    favorite ? l10n_util::GetStringUTF16(
                                   IDS_COMPASS_SIDEBAR_MENU_SET_FAVORITE_URL)
                             : l10n_util::GetStringUTF16(
                                   IDS_COMPASS_SIDEBAR_MENU_SET_PINNED_URL));
      menu->AddItem(kBackToSavedUrl,
                    favorite
                        ? l10n_util::GetStringUTF16(
                              IDS_COMPASS_SIDEBAR_MENU_BACK_TO_FAVORITE_URL)
                        : l10n_util::GetStringUTF16(
                              IDS_COMPASS_SIDEBAR_MENU_BACK_TO_PINNED_URL));
    }
    // With "Let Claude make plugins": a preview of this site's pages.
    if (live && node->url().SchemeIsHTTPOrHTTPS() &&
        browser_->GetProfile()->GetPrefs()->GetBoolean(
            claude::prefs::kPlugins)) {
      menu->AddItem(kMakePreview, l10n_util::GetStringFUTF16(
                                      IDS_COMPASS_SIDEBAR_MENU_MAKE_PREVIEW,
                                      base::UTF8ToUTF16(node->url().host())));
    }
    if (live) {
      menu->AddItem(kReload,
                    l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_RELOAD));
      // Its speaker mutes it while it plays; this mutes it before it does.
      if (registry().GetAudioState(node) ==
          LiveTabRegistry::AudioState::kMuted) {
        menu->AddItem(kUnmute, l10n_util::GetStringUTF16(
                                   IDS_COMPASS_SIDEBAR_MENU_UNMUTE_TAB));
      } else {
        menu->AddItem(kMute, l10n_util::GetStringUTF16(
                                 IDS_COMPASS_SIDEBAR_MENU_MUTE_TAB));
      }
    }
  }
  if (entry && !favorite) {
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
    if (pin_item) {
      menu->AddItem(
          pinned ? kUnpin : kPin,
          pinned ? l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_UNPIN)
                 : l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_PIN));
    }
    if (node->type() == SidebarNode::Type::kTab) {
      menu->AddItem(
          kAddToFavorites,
          l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_ADD_TO_FAVORITES));
    }
    menu->AddMoveToSpace(space_list.spaces(), space_model());
  }
  menu->AddSeparator(ui::NORMAL_SEPARATOR);
  if (live) {
    menu->AddItem(
        kCloseTab,
        node->type() == SidebarNode::Type::kSplit
            ? l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_CLOSE_SPLIT)
            : l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_MENU_CLOSE_TAB));
  }
  if (favorite) {
    menu->AddItem(kRemove, l10n_util::GetStringUTF16(
                               IDS_COMPASS_SIDEBAR_MENU_REMOVE_FROM_FAVORITES));
  } else if (pinned && entry) {
    menu->AddItem(kRemove, l10n_util::GetStringUTF16(
                               IDS_COMPASS_SIDEBAR_MENU_REMOVE_FROM_PINNED));
  } else if (!live) {
    // Closing an unloaded Today entry removes it, as its × does.
    menu->AddItem(kCloseTab, l10n_util::GetStringUTF16(IDS_COMPASS_REMOVE));
  }
  return menu;
}

void SidebarPageHandler::ExecuteMenuCommand(const std::string& id,
                                            int command) {
  SidebarNode* node = FindNode(id);
  if (!node) {
    return;
  }
  if (command == kShare) {
    // A folder, or an easel's tab (docs/design/sync.md, "Rooms: clients").
    // The bubble points at the row the menu was opened on.
    SidebarContextMenu* menu = context_menu();
    if (!menu) {
      return;
    }
    const gfx::Rect row = menu->ToScreen(
        gfx::Rect(0, menu->last_point().y() - kShareAnchorHalfHeight,
                  page_contents_->GetContainerBounds().width(),
                  2 * kShareAnchorHalfHeight));
    if (node->type() == SidebarNode::Type::kFolder) {
      ShowShareBubble(browser_, "folder", node->id().AsLowercaseString(),
                      row);
    } else if (std::string easel = rooms::EaselOfTab(*node); !easel.empty()) {
      ShowShareBubble(browser_, "easel", easel, row);
    }
    return;
  }
  // A viewer's shared items stay as the room has them.
  switch (command) {
    case kCopyLink:
    case kReload:
    case kMute:
    case kUnmute:
    case kCloseTab:
    case kOpenInSplit:
    case kSeparate:
    case kRefreshLiveFolder:
    case kMarkAllSeen:
      break;
    default:
      if (IsViewOnly(node)) {
        return;
      }
  }
  if (command == kDescribeLiveFolder) {
    if (claude::ClaudePanel* panel = claude::ClaudePanel::From(browser_)) {
      panel->Ask(
          "Help me make a live folder: a plugin that keeps a sidebar folder "
          "filled with links. Ask me what it should list and from where, "
          "write it, test it, and save it; then I'll add the folder with New "
          "Live Folder.");
    }
    return;
  }
  if (command == kMakePreview) {
    // Claude works on the tab: it's shown, then asked.
    ActivateNode(id);
    if (claude::ClaudePanel* panel = claude::ClaudePanel::From(browser_)) {
      panel->Ask(base::StrCat(
          {"Make a tab preview plugin for ", node->url().host(),
           ": when I hover this tab in the sidebar, show what matters on "
           "this page at a glance (what's new, next or waiting for me). Look "
           "at the page, tell me what you'd show, then test it before "
           "saving."}));
    }
    return;
  }
  SpaceModel& space = space_model();
  switch (command) {
    case kCopyLink:
      ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste)
          .WriteText(base::UTF8ToUTF16(
              LinkToCopy(browser_->GetProfile()->GetPrefs(), node->url())
                  .spec()));
      break;
    case kDuplicate:
      if (node->type() == SidebarNode::Type::kTab) {
        DuplicateNode(node);
      }
      break;
    case kReload:
      if (tabs::TabInterface* tab = registry().GetTab(node)) {
        // A site in development mode reloads from the network
        // (docs/design/dev-mode.md).
        content::WebContents* contents = tab->GetContents();
        contents->GetController().Reload(
            dev_mode::ShouldBypassCache(contents)
                ? content::ReloadType::BYPASSING_CACHE
                : content::ReloadType::NORMAL,
            /*check_for_repost=*/true);
      }
      break;
    case kPin:
      // To the end of Pinned.
      if (space.Contains(node) && space.CanMove(node, space.pinned_root())) {
        space.Move(node, space.pinned_root(),
                   space.pinned_root()->children().size());
      }
      break;
    case kUnpin:
      // To the top of Today, next to where it was.
      if (space.Contains(node) && space.CanMove(node, space.today_root())) {
        space.Move(node, space.today_root(), 0);
      }
      break;
    case kAddToFavorites:
      if (space.Contains(node)) {
        MoveEntry(browser_->GetProfile(), node, favorites(),
                  favorites().pinned_root(),
                  favorites().pinned_root()->children().size());
      }
      break;
    case kNewFolder:
      if (node->type() == SidebarNode::Type::kFolder) {
        space.AddFolder(
            node, node->children().size(),
            l10n_util::GetStringUTF16(IDS_COMPASS_SIDEBAR_CONTEXT_NEW_FOLDER));
      }
      break;
    case kUndoName:
      if (node->type() == SidebarNode::Type::kFolder) {
        GetModelOf(node).UndoFolderName(node);
      }
      break;
    case kSuggestName:
      if (node->type() == SidebarNode::Type::kFolder) {
        if (LinkFolderNamer* namer = namer_observation_.GetSource()) {
          namer->Suggest(node->id());
        }
      }
      break;
    case kRename:
      if (node->type() == SidebarNode::Type::kFolder ||
          node->type() == SidebarNode::Type::kTab) {
        page_->StartRename(id);
      }
      break;
    case kOpenInSplit: {
      SidebarNode* shown =
          bridge().GetShownNode(tab_strip_model()->GetActiveTab());
      if (shown && shown != node && space.Contains(shown) &&
          space.Contains(node) && space.CanSplit(shown, node)) {
        space.Split(shown, node);
        // Brings the other pane, and the bridge splits the window.
        ActivateNode(id);
      }
      break;
    }
    case kSeparate: {
      SidebarNode* split = node->type() == SidebarNode::Type::kSplit
                               ? node
                               : node->parent();
      if (split->type() == SidebarNode::Type::kSplit && space.Contains(split)) {
        space.Unsplit(split);
      }
      break;
    }
    case kMute:
    case kUnmute:
      SetNodeMuted(id, command == kMute);
      break;
    case kSetSavedUrl:
      if (node->type() == SidebarNode::Type::kTab &&
          !node->saved_url().is_empty() && node->url().is_valid()) {
        GetModelOf(node).SetSavedUrl(node, node->url(), node->title());
      }
      break;
    case kBackToSavedUrl:
      if (node->type() != SidebarNode::Type::kTab ||
          node->saved_url().is_empty()) {
        break;
      }
      if (tabs::TabInterface* tab = registry().GetTab(node)) {
        // The bridge follows the navigation to the node.
        tab->GetContents()->GetController().LoadURL(
            node->saved_url(), content::Referrer(),
            ui::PAGE_TRANSITION_AUTO_BOOKMARK, std::string());
      } else {
        SpaceModel& model = GetModelOf(node);
        model.SetUrl(node, node->saved_url());
        model.SetTitle(node, node->saved_title());
      }
      break;
    case kCloseTab:
      CloseNode(id);
      break;
    case kRemove:
      DeleteNode(id);
      break;
    case kNewLiveFolder:
      if (space.CanAddLiveFolder(node)) {
        page_->ShowLiveFolderEditor(id, std::nullopt);
      }
      break;
    case kRefreshLiveFolder:
      RefreshLiveFolder(id);
      break;
    case kEditLiveSource:
      if (node->is_live_folder()) {
        page_->ShowLiveFolderEditor(std::nullopt, id);
      }
      break;
    case kMarkAllSeen:
      if (node->is_live_folder()) {
        GetModelOf(node).MarkAllSeen(node);
      }
      break;
    case kConvertToPlainFolder:
      if (node->is_live_folder()) {
        GetModelOf(node).ConvertToPlainFolder(node);
      }
      break;
  }
}

void SidebarPageHandler::SendSwipe(sidebar::mojom::SwipePhase phase,
                                   float delta_x) {
  page_->Swipe(phase, delta_x);
}

// static
void SidebarPageHandler::SetSendTabModelForTesting(
    send_tab_to_self::SendTabToSelfModel* model) {
  g_send_tab_model_for_testing = model;
}

send_tab_to_self::SendTabToSelfModel* SidebarPageHandler::send_tab_model() {
  if (g_send_tab_model_for_testing) {
    return g_send_tab_model_for_testing;
  }
  send_tab_to_self::SendTabToSelfSyncService* service =
      SendTabToSelfSyncServiceFactory::GetForProfile(browser_->GetProfile());
  return service ? service->GetSendTabToSelfModel() : nullptr;
}

std::vector<syncer::RemoteDeviceInfo>
SidebarPageHandler::GetSendTargets(const GURL& url) {
  if (!send_tab_to_self::SendTabToSelfEntry::IsValidUrl(url)) {
    return {};
  }
  if (!g_send_tab_model_for_testing) {
    // Sync must offer it: the chain is on and some other device takes tabs.
    send_tab_to_self::SendTabToSelfSyncService* service =
        SendTabToSelfSyncServiceFactory::GetForProfile(browser_->GetProfile());
    if (!service || service->GetEntryPointDisplayReason(url) !=
                        send_tab_to_self::EntryPointDisplayReason::
                            kOfferFeature) {
      return {};
    }
  }
  send_tab_to_self::SendTabToSelfModel* model = send_tab_model();
  if (!model) {
    return {};
  }
  std::vector<syncer::RemoteDeviceInfo> devices =
      model->GetTargetDeviceInfoSortedList();
  if (devices.size() > kMaxSendTargets) {
    devices.resize(kMaxSendTargets);
  }
  return devices;
}

void SidebarPageHandler::SendNodeToDevice(
    const std::string& id,
    const syncer::RemoteDeviceInfo& device) {
  SidebarNode* node = FindNode(id);
  send_tab_to_self::SendTabToSelfModel* model = send_tab_model();
  if (!node || node->type() != SidebarNode::Type::kTab || !model) {
    return;
  }
  // The live tab's title if it has one, else the saved one.
  std::u16string title = node->title();
  if (tabs::TabInterface* tab = registry().GetTab(node)) {
    title = tab->GetContents()->GetTitle();
  }
  // The window's page shows the toast.
  base::WeakPtr<content::WebContents> shown;
  if (tabs::TabInterface* active = tab_strip_model()->GetActiveTab()) {
    shown = active->GetContents()->GetWeakPtr();
  }
  model->SendEntry(
      node->url(), base::UTF16ToUTF8(title), device.cache_guid,
      send_tab_to_self::PageContext(), send_tab_to_self::NavigationHistory(),
      base::BindOnce(&send_tab_to_self::OnSendTabToDeviceComplete, shown,
                     device.device_name, device.form_factor),
      send_tab_to_self::ShareEntryPoint::kTabMenu);
}

void SidebarPageHandler::OnNodeAdded(const SidebarNode& node) {
  page_->NodeAdded(node.parent()->id().AsLowercaseString(),
                   node.parent()->GetIndexOf(&node), ToMojo(node));
}

void SidebarPageHandler::OnNodeMoved(const SidebarNode& node,
                                     const SidebarNode& old_parent,
                                     size_t old_index) {
  page_->NodeMoved(node.id().AsLowercaseString(),
                   node.parent()->id().AsLowercaseString(),
                   node.parent()->GetIndexOf(&node));
  // An entry dragged out of its live folder is an ordinary tab now.
  if (node.type() == SidebarNode::Type::kTab) {
    page_->NodeLiveItemChanged(node.id().AsLowercaseString(),
                               node.is_live_item(), node.unseen());
  }
}

void SidebarPageHandler::OnNodeChanged(const SidebarNode& node) {
  page_->NodeChanged(node.id().AsLowercaseString(),
                     base::UTF16ToUTF8(node.title()),
                     base::UTF16ToUTF8(node.custom_title()), node.url(),
                     node.collapsed(), node.tab_tree());
  if (node.type() == SidebarNode::Type::kTab) {
    page_->NodeLiveItemChanged(node.id().AsLowercaseString(),
                               node.is_live_item(), node.unseen());
  } else if (node.type() == SidebarNode::Type::kFolder) {
    page_->LiveFolderChanged(node.id().AsLowercaseString(),
                             ToMojoLiveFolder(node));
  }
}

void SidebarPageHandler::OnNodeShortTitleChanged(const SidebarNode& node) {
  page_->NodeShortTitleChanged(node.id().AsLowercaseString(),
                               base::UTF16ToUTF8(node.short_title()));
}

void SidebarPageHandler::TidyToday() {
  if (TodayTidier* tidier = tidier_observation_.GetSource()) {
    tidier->Tidy(space_model());
  }
}

void SidebarPageHandler::UndoTidy(int32_t undo_tidy_id) {
  if (TodayTidier* tidier = tidier_observation_.GetSource()) {
    tidier->Undo(undo_tidy_id);
  }
}

void SidebarPageHandler::OnTidyBusyChanged(const base::Uuid& space_id) {
  if (observed_space_ && space_id == space_model().id()) {
    SendTidyState();
  }
}

void SidebarPageHandler::OnTidied(const base::Uuid& space_id,
                                  int undo_id,
                                  size_t folders) {
  if (!observed_space_ || space_id != space_model().id()) {
    return;
  }
  page_->ShowToast(
      l10n_util::GetPluralStringFUTF8(IDS_COMPASS_SIDEBAR_TIDIED_TOAST,
                                      static_cast<int>(folders)),
      undo_id);
}

void SidebarPageHandler::OnTidyFailed(const base::Uuid& space_id,
                                      const std::string& message) {
  if (observed_space_ && space_id == space_model().id()) {
    page_->ShowToast(message, 0);
  }
}

void SidebarPageHandler::SendTidyState(bool force) {
  if (!observed_space_) {
    return;
  }
  TodayTidier* tidier = tidier_observation_.GetSource();
  const bool available = tidier && tidier->IsEnabled() &&
                         space_model().kind() == SpaceModel::Kind::kSpace;
  const bool busy = tidier && tidier->IsBusy(space_model().id());
  const std::pair<bool, bool> state(available, busy);
  if (!force && tidy_state_ == state) {
    return;
  }
  tidy_state_ = state;
  page_->TidyChanged(available, busy);
}

void SidebarPageHandler::OnFolderNamed(const base::Uuid& folder_id) {
  SidebarNode* folder = space_model().FindById(folder_id);
  if (folder && observed_space_ == &space_model()) {
    page_->FolderNamed(folder_id.AsLowercaseString());
  }
}

bool SidebarPageHandler::CanSuggestNames() {
  Profile* profile = browser_->GetProfile();
  if (profile->IsOffTheRecord()) {
    return false;
  }
  claude::ClaudeQuick* quick = claude::ClaudeQuick::Get(profile);
  return quick && quick->IsEnabled(claude::prefs::kNameLinkFolders);
}

void SidebarPageHandler::OnNodeWillBeRemoved(const SidebarNode& node) {
  page_->NodeRemoved(node.id().AsLowercaseString());
}

void SidebarPageHandler::OnNodeWillMoveToSpace(const SidebarNode& node) {
  page_->NodeRemoved(node.id().AsLowercaseString());
}

void SidebarPageHandler::OnSpaceAdded(SpaceModel& space) {
  // The directory relays the list's changes too.
  if (!uses_directory_) {
    SendSpaces();
  }
}

void SidebarPageHandler::OnSpaceUpdated(const SpaceModel& space) {
  if (!uses_directory_) {
    SendSpaces();
  }
}

void SidebarPageHandler::OnSpaceMoved(const SpaceModel& space) {
  if (!uses_directory_) {
    SendSpaces();
  }
}

void SidebarPageHandler::OnSpaceWillBeRemoved(SpaceModel& space) {
  // Windows showing it have switched away; the list still has it.
  CHECK_NE(&space, observed_space_);
  if (uses_directory_) {
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&SidebarPageHandler::SendSpaces,
                                weak_factory_.GetWeakPtr()));
}

void SidebarPageHandler::OnDirectoryChanged() {
  SendSpaces();
}

void SidebarPageHandler::OnProfileAdded(const base::FilePath& profile_path) {
  SendSpaces();
}

void SidebarPageHandler::OnProfileWasRemoved(
    const base::FilePath& profile_path,
    const std::u16string& profile_name) {
  SendSpaces();
}

void SidebarPageHandler::OnProfileNameChanged(
    const base::FilePath& profile_path,
    const std::u16string& old_profile_name) {
  SendSpaces();
}

void SidebarPageHandler::OnProfileIsOmittedChanged(
    const base::FilePath& profile_path) {
  SendSpaces();
}

void SidebarPageHandler::OnSpaceSwitched() {
  observed_space_->RemoveObserver(this);
  observed_space_ = &space_model();
  observed_space_->AddObserver(this);
  RequestTree();
}

void SidebarPageHandler::OnSwappedAway() {
  // The window has no bridge left to act through, and closes in a moment.
  // Its space can go before it does (moved to another profile). The page's
  // calls wait for the new window's handler (TakePipes()).
  receiver_.Pause();
  StopObserving();
}

std::pair<mojo::PendingReceiver<sidebar::mojom::PageHandler>,
          mojo::PendingRemote<sidebar::mojom::Page>>
SidebarPageHandler::TakePipes() {
  StopObserving();
  return {receiver_.Unbind(), page_.Unbind()};
}

void SidebarPageHandler::OnNodeLiveChanged(const SidebarNode& node, bool live) {
  page_->NodeLiveChanged(node.id().AsLowercaseString(), live);
}

void SidebarPageHandler::OnClaudeActingChanged(
    content::WebContents* contents) {
  // Any window's tab: the node shows in every window of its space.
  const tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(contents);
  const SidebarNode* node = tab ? registry().GetNode(tab) : nullptr;
  if (node) {
    page_->NodeClaudeChanged(node->id().AsLowercaseString(),
                             claude::ClaudeActivity::IsActing(contents));
  }
}

void SidebarPageHandler::OnTabCountsChanged() {
  // Every tab row of the window's space and the favorites; the page ignores
  // what didn't change.
  auto visit = [this](auto& self, const SidebarNode& node) -> void {
    if (node.type() == SidebarNode::Type::kTab) {
      page_->NodeBadgeChanged(node.id().AsLowercaseString(),
                              BadgeCount(node, registry()));
    }
    for (const auto& child : node.children()) {
      self(self, *child);
    }
  };
  visit(visit, *favorites().pinned_root());
  visit(visit, *space_model().pinned_root());
  visit(visit, *space_model().today_root());
}

int32_t SidebarPageHandler::BadgeCount(const SidebarNode& node,
                                       const LiveTabRegistry& registry) {
  LiveFolderService* service = LiveFolderService::Get(browser_->GetProfile());
  const tabs::TabInterface* tab = registry.GetTab(&node);
  if (!service || !tab) {
    return 0;
  }
  const TabCount count = service->previews().GetCount(
      tab->GetContents()->GetLastCommittedURL());
  return count.some && count.count <= 0 ? -1 : count.count;
}

void SidebarPageHandler::OnNodeAudioChanged(const SidebarNode& node) {
  page_->NodeAudioChanged(node.id().AsLowercaseString(),
                          ToMojoAudio(registry().GetAudioState(&node)));
}

void SidebarPageHandler::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  // The bridge observed the strip first, so a new tab already has its node.
  // A switch sends the whole tree when it's done.
  if (selection.active_tab_changed() && !features_->is_switching()) {
    page_->ActiveNodeChanged(GetActiveNodeId());
  }
}

TabStripModel* SidebarPageHandler::tab_strip_model() {
  return browser_->GetTabStripModel();
}

SpaceModel& SidebarPageHandler::space_model() {
  return features_->space_model();
}

SpaceModel& SidebarPageHandler::favorites() {
  return features_->service().space_list().favorites();
}

SpaceModel& SidebarPageHandler::GetModelOf(const SidebarNode* node) {
  return favorites().Contains(node) ? favorites() : space_model();
}

TabStripBridge& SidebarPageHandler::bridge() {
  return features_->tab_strip_bridge();
}

LiveTabRegistry& SidebarPageHandler::registry() {
  return features_->registry();
}

ClosedEntries& SidebarPageHandler::closed_entries() {
  return features_->service().closed_entries();
}

void SidebarPageHandler::SendSpaces() {
  Profile* own_profile = browser_->GetProfile();
  std::vector<SpaceDirectory::Entry> entries;
  if (SpaceDirectory* directory = this->directory()) {
    entries = directory->GetSpaces();
  } else {
    for (const auto& space : features_->service().space_list().spaces()) {
      entries.push_back({own_profile, space.get()});
    }
  }
  std::vector<sidebar::mojom::SpacePtr> spaces;
  for (const SpaceDirectory::Entry& entry : entries) {
    spaces.push_back(ToMojoSpace(entry, own_profile));
  }
  // The profiles a space can be assigned to, as the profile menu lists them.
  std::vector<sidebar::mojom::ProfileOptionPtr> profiles;
  if (directory()) {
    profiles = GetProfileOptions();
  }
  page_->SetSpaces(std::move(spaces), space_model().id().AsLowercaseString(),
                   std::move(profiles));
}

void SidebarPageHandler::SendNowPlaying() {
  std::vector<sidebar::mojom::NowPlayingPtr> items;
  if (now_playing_) {
    for (const NowPlayingTracker::Item& item : now_playing_->items()) {
      auto mojo_item = sidebar::mojom::NowPlaying::New();
      mojo_item->id = item.id;
      mojo_item->title = base::UTF16ToUTF8(item.title);
      mojo_item->url = item.url;
      mojo_item->playing = item.playing;
      mojo_item->muted = item.muted;
      mojo_item->can_play_pause = item.can_play_pause;
      mojo_item->can_previous = item.can_previous;
      mojo_item->can_next = item.can_next;
      mojo_item->can_picture_in_picture = item.can_picture_in_picture;
      items.push_back(std::move(mojo_item));
    }
  }
  page_->SetNowPlaying(std::move(items));
}

void SidebarPageHandler::SendUpdateReady() {
  page_->SetUpdateReady(update_notice_ && update_notice_->ShouldShow());
}

void SidebarPageHandler::EditLiveFolder(
    const std::optional<std::string>& parent_id,
    const std::optional<std::string>& folder_id,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height) {
  SidebarContextMenu* menu = context_menu();
  if (!menu) {
    return;
  }
  // Checked again when it saves.
  if (folder_id) {
    SidebarNode* folder = FindNode(*folder_id);
    if (!folder || !folder->is_live_folder()) {
      return;
    }
  } else if (parent_id) {
    SidebarNode* parent = FindNode(*parent_id);
    if (!parent || !space_model().CanAddLiveFolder(parent)) {
      return;
    }
  } else {
    return;
  }
  ShowLiveFolderEditorBubble(browser_, parent_id, folder_id,
                             menu->ToScreen(gfx::Rect(x, y, width, height)));
}

void SidebarPageHandler::RefreshLiveFolder(const std::string& id) {
  SidebarNode* node = FindNode(id);
  LiveFolderService* service = LiveFolderService::Get(browser_->GetProfile());
  if (node && node->is_live_folder() && service) {
    service->Refresh(node->id());
  }
}

void SidebarPageHandler::OnLiveFolderStateChanged(const base::Uuid& folder_id) {
  if (!observed_space_ || !space_model().FindById(folder_id)) {
    return;
  }
  LiveFolderService* service = LiveFolderService::Get(browser_->GetProfile());
  page_->LiveFolderStateChanged(
      folder_id.AsLowercaseString(),
      ToMojoLiveState(service ? service->GetState(folder_id) : nullptr));
}

sidebar::mojom::NodePtr SidebarPageHandler::ToMojo(const SidebarNode& node) {
  return ToMojo(node, registry());
}

sidebar::mojom::NodePtr SidebarPageHandler::ToMojo(
    const SidebarNode& node,
    const LiveTabRegistry& registry) {
  auto result = sidebar::mojom::Node::New();
  result->id = node.id().AsLowercaseString();
  result->type = ToMojoType(node.type());
  result->title = base::UTF16ToUTF8(node.title());
  result->custom_title = base::UTF16ToUTF8(node.custom_title());
  result->short_title = base::UTF16ToUTF8(node.short_title());
  result->url = node.url();
  // Live in any window: activating it here moves it here.
  result->live = registry.GetTab(&node) != nullptr;
  result->audio = ToMojoAudio(registry.GetAudioState(&node));
  const tabs::TabInterface* tab = registry.GetTab(&node);
  result->claude =
      tab && claude::ClaudeActivity::IsActing(tab->GetContents());
  result->badge_count =
      node.type() == SidebarNode::Type::kTab ? BadgeCount(node, registry) : 0;
  result->collapsed = node.collapsed();
  result->tab_tree = node.tab_tree();
  result->live_folder = ToMojoLiveFolder(node);
  result->live_item = node.is_live_item();
  result->unseen = node.unseen();
  if (rooms::RoomService* rooms =
          browser_->GetProfile()->IsOffTheRecord()
              ? nullptr
              : rooms::RoomService::GetIfExists(browser_->GetProfile())) {
    const rooms::SharedState state = rooms->GetNodeState(node);
    result->view_only = state.read_only;
    // The badge goes on the shared item itself, not what's in it.
    const bool parent_shared = node.parent() &&
                               node.parent()->type() !=
                                   SidebarNode::Type::kRoot &&
                               rooms->GetNodeState(*node.parent()).shared;
    if (state.shared && !parent_shared) {
      result->shared = true;
      result->shared_tooltip = state.tooltip;
    }
  }
  for (const auto& child : node.children()) {
    result->children.push_back(ToMojo(*child, registry));
  }
  return result;
}

SidebarNode* SidebarPageHandler::FindNode(const std::string& id) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  if (!uuid.is_valid()) {
    return nullptr;
  }
  SidebarNode* node = space_model().FindById(uuid);
  return node ? node : favorites().FindById(uuid);
}

SpaceModel* SidebarPageHandler::FindSpace(const std::string& id) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  return uuid.is_valid() ? features_->service().space_list().FindSpace(uuid)
                         : nullptr;
}

SpaceDirectory::Entry SidebarPageHandler::FindAnySpace(const std::string& id) {
  if (SpaceModel* space = FindSpace(id)) {
    return {browser_->GetProfile(), space};
  }
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  SpaceDirectory* directory = this->directory();
  return uuid.is_valid() && directory ? directory->FindSpace(uuid)
                                      : SpaceDirectory::Entry();
}

SpaceDirectory* SidebarPageHandler::directory() {
  // Gone with the profile manager, during shutdown.
  return uses_directory_ ? SpaceDirectory::GetIfExists() : nullptr;
}

std::optional<std::string> SidebarPageHandler::GetActiveNodeId() {
  // A placeholder shows its node as active.
  const SidebarNode* node =
      bridge().GetShownNode(tab_strip_model()->GetActiveTab());
  if (!node) {
    return std::nullopt;
  }
  return node->id().AsLowercaseString();
}

SidebarContextMenu* SidebarPageHandler::context_menu() {
  return page_contents_ ? SidebarContextMenu::FromWebContents(page_contents_)
                        : nullptr;
}

SidebarHoverCard* SidebarPageHandler::hover_card() {
  return page_contents_ ? SidebarHoverCard::FromWebContents(page_contents_)
                        : nullptr;
}

void SidebarPageHandler::DuplicateNode(SidebarNode* node) {
  // A tab here is copied with its history; the bridge puts the copy's entry
  // after the tab's.
  if (tabs::TabInterface* tab = bridge().GetTab(node)) {
    const int index = tab_strip_model()->GetIndexOfTab(tab);
    if (chrome::CanDuplicateTabAt(browser_, index)) {
      chrome::DuplicateTabAt(browser_, index);
    }
    return;
  }
  // Unloaded, or live in another window: a new entry for its URL, after it
  // in Today, or first.
  SidebarNode* today = space_model().today_root();
  const size_t index =
      node->parent() == today ? today->GetIndexOf(node) + 1 : 0;
  SidebarNode* copy =
      space_model().AddTab(today, index, node->url(), node->title());
  if (tabs::TabInterface* live = bridge().OpenNode(copy)) {
    tab_strip_model()->ActivateTabAt(tab_strip_model()->GetIndexOfTab(live));
  }
}

void SidebarPageHandler::CloseTabNode(SidebarNode* tab_node, bool recorded) {
  ClosedEntries& closed = closed_entries();
  // Open in a window, this one or another: it closes there, as Cmd+W would,
  // and that window's bridge records it (a pinned tab stays, unloaded).
  if (registry().GetTab(tab_node)) {
    if (recorded) {
      closed.ExpectClose(tab_node->id());
    }
    if (CloseLiveTab(registry(), *tab_node)) {
      return;
    }
    if (recorded) {
      closed.TakeExpectedClose(tab_node->id());
    }
  }
  SpaceModel& model = GetModelOf(tab_node);
  if (!recorded) {
    closed.RecordRemoval(model, *tab_node);
  }
  model.Remove(tab_node);
}

void SidebarPageHandler::EditSpace(const std::string& space_id,
                                   int32_t x,
                                   int32_t y,
                                   int32_t width,
                                   int32_t height) {
  SidebarContextMenu* menu = context_menu();
  if (FindAnySpace(space_id).space && menu) {
    ShowSpaceEditorBubble(browser_, space_id,
                          menu->ToScreen(gfx::Rect(x, y, width, height)));
  }
}
void SidebarPageHandler::GetRoomNotices(GetRoomNoticesCallback callback) {
  std::vector<sidebar::mojom::RoomNoticePtr> notices;
  if (rooms::RoomService* rooms =
          browser_->GetProfile()->IsOffTheRecord()
              ? nullptr
              : rooms::RoomService::GetIfExists(browser_->GetProfile())) {
    for (const rooms::RemovedNotice& notice : rooms->GetRemovedNotices()) {
      notices.push_back(sidebar::mojom::RoomNotice::New(
          notice.room_id, rooms::RemovedNoticeText(notice)));
    }
  }
  std::move(callback).Run(std::move(notices));
}

void SidebarPageHandler::DismissRoomNotice(const std::string& room_id) {
  if (rooms::RoomService* rooms =
          rooms::RoomService::GetIfExists(browser_->GetProfile())) {
    rooms->DismissRemovedNotice(room_id);
  }
}

void SidebarPageHandler::OnRoomsChanged() {
  RequestTree();
}

bool SidebarPageHandler::CanShare() {
  rooms::RoomService* rooms =
      rooms::RoomService::GetIfExists(browser_->GetProfile());
  return rooms && rooms->IsAvailable();
}

bool SidebarPageHandler::IsShared(const SidebarNode* node) {
  rooms::RoomService* rooms =
      rooms::RoomService::GetIfExists(browser_->GetProfile());
  return node && rooms && rooms->GetNodeState(*node).shared;
}

bool SidebarPageHandler::IsViewOnly(const SidebarNode* node) {
  rooms::RoomService* rooms =
      rooms::RoomService::GetIfExists(browser_->GetProfile());
  return node && rooms && rooms->GetNodeState(*node).read_only;
}

SpaceModel* SidebarPageHandler::FindEditableSpace(const std::string& id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  return entry.space && !IsViewOnlySpace(entry.profile, *entry.space)
             ? entry.space.get()
             : nullptr;
}

}  // namespace compass
