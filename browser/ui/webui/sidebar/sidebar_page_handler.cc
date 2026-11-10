// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sidebar/sidebar_page_handler.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "compass/browser/ui/live_folders/live_plugin_registry.h"
#include "base/check_op.h"
#include "base/containers/fixed_flat_map.h"
#include "base/functional/bind.h"
#include "base/notreached.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_muted_utils.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "compass/browser/ui/views/command_bar/command_bar.h"
#include "compass/browser/ui/views/little_window/little_window.h"
#include "compass/browser/ui/webui/sidebar/now_playing_tracker.h"
#include "compass/browser/ui/views/sidebar/sidebar_context_menu.h"
#include "compass/browser/ui/views/sidebar/sidebar_hover_card.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/sync/send_tab_to_self_sync_service_factory.h"
#include "chrome/browser/ui/send_tab_to_self/send_tab_to_self_util.h"
#include "components/send_tab_to_self/entry_point_display_reason.h"
#include "components/send_tab_to_self/metrics_util.h"
#include "components/send_tab_to_self/page_context.h"
#include "components/send_tab_to_self/send_tab_to_self_entry.h"
#include "components/send_tab_to_self/send_tab_to_self_model.h"
#include "components/send_tab_to_self/send_tab_to_self_sync_service.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "compass/browser/sidebar/closed_entries.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/undo_close.h"
#include "compass/browser/window_sync/window_features.h"
#include "compass/common/compass_url.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/models/menu_separator_types.h"
#include "ui/base/page_transition_types.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/point.h"

namespace compass {

namespace {

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

sidebar::mojom::LivePluginPtr ToMojoPlugin(const LivePlugin& plugin,
                                           const LiveFolderStore* store) {
  const LivePluginManifest& manifest = plugin.manifest;
  auto result = sidebar::mojom::LivePlugin::New();
  result->id = manifest.id;
  result->name = manifest.name;
  result->description = manifest.description;
  result->icon_url = plugin.IconUrl();
  using Type = LivePluginManifest::ConfigField::Type;
  for (const auto& field : manifest.config) {
    auto f = sidebar::mojom::LiveConfigField::New();
    f->key = field.key;
    switch (field.type) {
      case Type::kString:
        f->type = "string";
        break;
      case Type::kUrl:
        f->type = "url";
        break;
      case Type::kNumber:
        f->type = "number";
        break;
      case Type::kBool:
        f->type = "bool";
        break;
      case Type::kSelect:
        f->type = "select";
        break;
    }
    f->label = field.label;
    f->placeholder = field.placeholder;
    f->optional = field.optional;
    if (field.default_value) {
      f->default_json = base::WriteJson(*field.default_value).value_or("");
    }
    f->min = field.min;
    f->max = field.max;
    for (const auto& option : field.options) {
      f->options.push_back(
          sidebar::mojom::LiveSelectOption::New(option.value, option.label));
    }
    result->config.push_back(std::move(f));
  }
  for (const auto& secret : manifest.secrets) {
    result->secrets.push_back(sidebar::mojom::LiveSecretField::New(
        secret.key, secret.label, secret.help, secret.optional,
        store && store->HasSecret(manifest.id, secret.key)));
  }
  // The manifest's patterns; placeholders show as the field they name.
  result->cookie_hosts = manifest.cookies;
  result->builtin = plugin.builtin;
  result->local = plugin.local;
  return result;
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

constexpr uint32_t kMaxArchiveAfterHours = 365 * 24;

send_tab_to_self::SendTabToSelfModel* g_send_tab_model_for_testing = nullptr;

// Null for grey.
sidebar::mojom::SpaceColorPtr ToMojoColor(SpaceColor color) {
  if (color.is_grey()) {
    return nullptr;
  }
  const SkColor tint = *color.tint();
  return sidebar::mojom::SpaceColor::New(
      SkColorGetR(tint), SkColorGetG(tint), SkColorGetB(tint));
}

// The handler casts between the two.
static_assert(static_cast<int>(sidebar::mojom::SpaceAppearance::kMaxValue) ==
              static_cast<int>(SpaceAppearance::kDark));

// Null for none.
sidebar::mojom::SpaceIconPtr ToMojoIcon(const SpaceIcon& icon) {
  switch (icon.kind()) {
    case SpaceIcon::Kind::kNone:
      return nullptr;
    case SpaceIcon::Kind::kEmoji:
      return sidebar::mojom::SpaceIcon::NewEmoji(icon.value());
    case SpaceIcon::Kind::kSymbol:
      return sidebar::mojom::SpaceIcon::NewSymbol(icon.value());
  }
  NOTREACHED();
}

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
      AddSubMenu(SidebarPageHandler::kMoveToSpace, u"Move to Space",
                 &spaces_menu_);
    }
  }

  // Adds "Send to Your Devices" with |devices|, if there are any.
  void AddSendToDevices(
      std::vector<send_tab_to_self::TargetDeviceInfo> devices) {
    devices_ = std::move(devices);
    for (size_t i = 0; i < devices_.size(); ++i) {
      devices_menu_.AddItem(SidebarPageHandler::kSendToDeviceFirst + i,
                            base::UTF8ToUTF16(devices_[i].device_name));
    }
    if (!devices_.empty()) {
      AddSubMenu(SidebarPageHandler::kSendToDevices, u"Send to Your Devices",
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
  std::vector<send_tab_to_self::TargetDeviceInfo> devices_;
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
  }
  if (page_contents_) {
    now_playing_ = std::make_unique<NowPlayingTracker>(
        browser_, base::BindRepeating(&SidebarPageHandler::SendNowPlaying,
                                      base::Unretained(this)));
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
}

void SidebarPageHandler::RequestTree() {
  SendSpaces();
  auto tree = sidebar::mojom::Tree::New();
  tree->favorites = ToMojo(*favorites().pinned_root());
  tree->pinned = ToMojo(*space_model().pinned_root());
  tree->today = ToMojo(*space_model().today_root());
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
  SendArchive();
  SendNowPlaying();
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
  SidebarNode* node = FindNode(id);
  if (!node) {
    return;
  }
  // Whatever closes is one step for Cmd+Z.
  ClosedEntries::ScopedStep step(closed_entries());
  switch (node->type()) {
    case SidebarNode::Type::kTab:
      CloseTabNode(node);
      break;
    case SidebarNode::Type::kSplit: {
      // A Today split comes back whole. A pinned one stays, and each pane
      // records what happens to it.
      const bool recorded = !space_model().pinned_root()->Contains(node);
      if (recorded) {
        closed_entries().RecordRemoval(space_model(), *node);
      }
      // Closing a pane can dissolve the split, so go by ID.
      std::vector<base::Uuid> panes;
      for (const auto& pane : node->children()) {
        panes.push_back(pane->id());
      }
      for (const base::Uuid& pane_id : panes) {
        if (SidebarNode* pane = space_model().FindById(pane_id)) {
          CloseTabNode(pane, recorded);
        }
      }
      break;
    }
    case SidebarNode::Type::kRoot:
    case SidebarNode::Type::kFolder:
      // Folders come with slice c.
      break;
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
  if (!parent || index > parent->children().size() ||
      (parent->type() != SidebarNode::Type::kRoot &&
       parent->type() != SidebarNode::Type::kFolder) ||
      !(url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile())) {
    return;
  }
  // The page's title replaces the URL once it loads.
  SidebarNode* node = GetModelOf(parent).AddTab(parent, index, url,
                                                base::UTF8ToUTF16(url.spec()));
  bridge().OpenNode(node);
}

void SidebarPageHandler::AddFolder(const std::string& parent_id,
                                   uint32_t index,
                                   const std::string& title) {
  SidebarNode* parent = FindNode(parent_id);
  if (!parent || index > parent->children().size() ||
      !space_model().Contains(parent) ||
      (parent->type() != SidebarNode::Type::kRoot &&
       parent->type() != SidebarNode::Type::kFolder)) {
    return;
  }
  space_model().AddFolder(parent, index, base::UTF8ToUTF16(title));
}

void SidebarPageHandler::RenameFolder(const std::string& id,
                                      const std::string& title) {
  SidebarNode* node = FindNode(id);
  if (node && node->type() == SidebarNode::Type::kFolder) {
    space_model().SetTitle(node, base::UTF8ToUTF16(title));
  }
}

void SidebarPageHandler::RenameTab(const std::string& id,
                                   const std::string& title) {
  SidebarNode* node = FindNode(id);
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
  if (node && node->type() == SidebarNode::Type::kFolder) {
    space_model().SetCollapsed(node, collapsed);
  }
}

void SidebarPageHandler::DeleteNode(const std::string& id) {
  SidebarNode* node = FindNode(id);
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
  SpaceList& list = features_->service().space_list();
  SpaceModel* space = list.AddSpace(
      list.spaces().size(),
      u"Space " + base::NumberToString16(list.spaces().size() + 1));
  features_->SwitchToSpace(*space);
}

void SidebarPageHandler::RenameSpace(const std::string& id,
                                     const std::string& name) {
  SpaceModel* space = FindAnySpace(id).space;
  std::u16string name16;
  base::TrimWhitespace(base::UTF8ToUTF16(name), base::TRIM_ALL, &name16);
  if (space && !name16.empty()) {
    space->SetName(std::move(name16));
  }
}

void SidebarPageHandler::SetSpaceColor(
    const std::string& id,
    sidebar::mojom::SpaceColorPtr color) {
  if (SpaceModel* space = FindAnySpace(id).space) {
    space->SetColor(color ? SpaceColor::FromRGB(color->red, color->green,
                                                color->blue)
                          : SpaceColor::kGrey);
  }
}

void SidebarPageHandler::SetSpaceGradient(
    const std::string& id,
    std::vector<sidebar::mojom::SpaceColorPtr> gradient) {
  SpaceModel* space = FindAnySpace(id).space;
  if (!space) {
    return;
  }
  std::vector<SpaceColor> colors;
  for (const sidebar::mojom::SpaceColorPtr& color : gradient) {
    colors.push_back(
        SpaceColor::FromRGB(color->red, color->green, color->blue));
  }
  space->SetGradient(std::move(colors));
}

void SidebarPageHandler::SetSpaceIntensity(const std::string& id,
                                           double intensity) {
  if (SpaceModel* space = FindAnySpace(id).space) {
    space->SetIntensity(intensity);
  }
}

void SidebarPageHandler::SetSpaceGrain(const std::string& id, double grain) {
  if (SpaceModel* space = FindAnySpace(id).space) {
    space->SetGrain(grain);
  }
}

void SidebarPageHandler::SetSpaceAppearance(
    const std::string& id,
    sidebar::mojom::SpaceAppearance appearance) {
  // Mojo has validated |appearance|.
  if (SpaceModel* space = FindAnySpace(id).space) {
    space->SetAppearance(static_cast<SpaceAppearance>(appearance));
  }
}

void SidebarPageHandler::SetSpaceIcon(const std::string& id,
                                      sidebar::mojom::SpaceIconPtr icon) {
  SpaceModel* space = FindAnySpace(id).space;
  if (!space) {
    return;
  }
  if (!icon) {
    space->SetIcon(SpaceIcon());
    return;
  }
  std::optional<SpaceIcon> valid =
      icon->is_emoji() ? SpaceIcon::Emoji(icon->get_emoji())
                       : SpaceIcon::Symbol(icon->get_symbol());
  if (valid) {
    space->SetIcon(std::move(*valid));
  }
}

void SidebarPageHandler::SetSpaceProfile(
    const std::string& id,
    const std::optional<std::string>& profile_id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (!entry.space || !directory() || !profile_manager) {
    return;
  }
  std::optional<base::FilePath> path;
  if (profile_id) {
    const base::FilePath name = base::FilePath::FromUTF8Unsafe(*profile_id);
    // A directory name, nothing more.
    if (name.empty() || name.BaseName() != name ||
        name.ReferencesParent()) {
      return;
    }
    path = profile_manager->user_data_dir().Append(name);
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
  SpaceModel* space = FindSpace(space_id);
  if (node && space) {
    MoveEntryToSpace(browser_->GetProfile(), node, *space);
  }
}

void SidebarPageHandler::SetArchiveAfter(const std::string& space_id,
                                         uint32_t hours) {
  SpaceModel* space = FindAnySpace(space_id).space;
  if (space && hours <= kMaxArchiveAfterHours) {
    space->SetArchiveAfter(GetArchiveHour() * static_cast<int64_t>(hours));
  }
}

void SidebarPageHandler::RestoreArchived(const std::string& id) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  if (!uuid.is_valid()) {
    return;
  }
  SpaceModel& space = space_model();
  SidebarNode* entry =
      space.Unarchive(uuid, space.today_root()->children().size());
  if (!entry) {
    return;
  }
  // A split opens its first pane.
  SidebarNode* tab = entry->type() == SidebarNode::Type::kSplit
                         ? entry->children()[0].get()
                         : entry;
  if (tabs::TabInterface* live = bridge().OpenNode(tab)) {
    tab_strip_model()->ActivateTabAt(tab_strip_model()->GetIndexOfTab(live));
  }
}

void SidebarPageHandler::RemoveArchived(const std::string& id) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  if (uuid.is_valid()) {
    space_model().RemoveArchived(uuid);
  }
}

void SidebarPageHandler::ClearArchive() {
  space_model().ClearArchive();
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
  const bool show_preview = tab && tab != tab_strip_model()->GetActiveTab();
  card->Show(data, row_top, row_height, show_preview);
}

void SidebarPageHandler::HideHoverCard() {
  if (SidebarHoverCard* card = hover_card()) {
    card->Hide();
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

std::unique_ptr<ui::SimpleMenuModel> SidebarPageHandler::CreateContextMenu(
    const std::string& id) {
  SidebarNode* node = FindNode(id);
  if (!node || node->type() == SidebarNode::Type::kRoot) {
    return nullptr;
  }
  auto menu = std::make_unique<NodeMenu>(weak_factory_.GetWeakPtr(), id);
  const bool favorite = favorites().Contains(node);
  const bool pinned = space_model().pinned_root()->Contains(node);
  const SpaceList& space_list = features_->service().space_list();

  if (node->type() == SidebarNode::Type::kFolder) {
    if (node->is_live_folder()) {
      menu->AddItem(kRefreshLiveFolder, u"Refresh");
      menu->AddItem(kEditLiveSource, u"Edit Source\u2026");
      menu->AddItem(kMarkAllSeen, u"Mark All Seen");
      menu->AddItem(kConvertToPlainFolder, u"Convert to Normal Folder");
      menu->AddSeparator(ui::NORMAL_SEPARATOR);
    } else {
      menu->AddItem(kNewFolder, u"New Folder");
      if (space_model().CanAddLiveFolder(node) &&
          LiveFolderService::Get(browser_->GetProfile())) {
        menu->AddItem(kNewLiveFolder, u"New Live Folder\u2026");
      }
    }
    menu->AddItem(kRename, u"Rename");
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
    menu->AddItem(pinned ? kUnpin : kPin, pinned ? u"Unpin" : u"Pin");
    menu->AddMoveToSpace(space_list.spaces(), space_model());
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
    menu->AddItem(kRemove, u"Delete Folder");
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
    menu->AddItem(kSeparate, u"Separate Tabs");
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
  } else if (entry && !favorite && shown && shown != node &&
             space_model().Contains(shown) &&
             space_model().CanSplit(shown, node)) {
    menu->AddItem(kOpenInSplit, u"Open in Split View");
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
  }
  if (node->type() == SidebarNode::Type::kTab) {
    // A favorite's tile shows no name.
    if (!favorite) {
      menu->AddItem(kRename, u"Rename");
    }
    menu->AddItem(kCopyLink, u"Copy Link");
    if (entry) {
      menu->AddItem(kDuplicate, u"Duplicate");
    }
    menu->AddSendToDevices(GetSendTargets(node->url()));
    // The node hears of its tab's navigations up to 200 ms late.
    if (tabs::TabInterface* tab = registry().GetTab(node)) {
      bridge().UpdateNode(tab);
    }
    if (!node->saved_url().is_empty() && node->url().is_valid() &&
        node->url() != node->saved_url()) {
      menu->AddItem(kSetSavedUrl, favorite ? u"Use This Page as Favorite URL"
                                           : u"Use This Page as Pinned URL");
      menu->AddItem(kBackToSavedUrl, favorite ? u"Back to Favorite URL"
                                              : u"Back to Pinned URL");
    }
    if (live) {
      menu->AddItem(kReload, u"Reload");
      // Its speaker mutes it while it plays; this mutes it before it does.
      if (registry().GetAudioState(node) ==
          LiveTabRegistry::AudioState::kMuted) {
        menu->AddItem(kUnmute, u"Unmute Tab");
      } else {
        menu->AddItem(kMute, u"Mute Tab");
      }
    }
  }
  if (entry && !favorite) {
    menu->AddSeparator(ui::NORMAL_SEPARATOR);
    menu->AddItem(pinned ? kUnpin : kPin, pinned ? u"Unpin" : u"Pin");
    if (node->type() == SidebarNode::Type::kTab) {
      menu->AddItem(kAddToFavorites, u"Add to Favorites");
    }
    menu->AddMoveToSpace(space_list.spaces(), space_model());
  }
  menu->AddSeparator(ui::NORMAL_SEPARATOR);
  if (live) {
    menu->AddItem(kCloseTab, node->type() == SidebarNode::Type::kSplit
                                 ? u"Close Split"
                                 : u"Close Tab");
  }
  if (favorite) {
    menu->AddItem(kRemove, u"Remove from Favorites");
  } else if (pinned && entry) {
    menu->AddItem(kRemove, u"Remove from Pinned");
  } else if (!live) {
    // Closing an unloaded Today entry removes it, as its × does.
    menu->AddItem(kCloseTab, u"Remove");
  }
  return menu;
}

void SidebarPageHandler::ExecuteMenuCommand(const std::string& id,
                                            int command) {
  SidebarNode* node = FindNode(id);
  if (!node) {
    return;
  }
  SpaceModel& space = space_model();
  switch (command) {
    case kCopyLink:
      ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste)
          .WriteText(base::UTF8ToUTF16(ToCompassURL(node->url()).spec()));
      break;
    case kDuplicate:
      if (node->type() == SidebarNode::Type::kTab) {
        DuplicateNode(node);
      }
      break;
    case kReload:
      if (tabs::TabInterface* tab = registry().GetTab(node)) {
        tab->GetContents()->GetController().Reload(content::ReloadType::NORMAL,
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
        space.AddFolder(node, node->children().size(), u"New Folder");
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

std::vector<send_tab_to_self::TargetDeviceInfo>
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
  std::vector<send_tab_to_self::TargetDeviceInfo> devices =
      model->GetTargetDeviceInfoSortedList();
  if (devices.size() > kMaxSendTargets) {
    devices.resize(kMaxSendTargets);
  }
  return devices;
}

void SidebarPageHandler::SendNodeToDevice(
    const std::string& id,
    const send_tab_to_self::TargetDeviceInfo& device) {
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
      base::BindOnce(
          [](base::WeakPtr<content::WebContents> contents,
             std::string device_name,
             syncer::DeviceInfo::FormFactor form_factor,
             send_tab_to_self::SendTabToSelfResult result) {
            if (!contents) {
              return;
            }
            switch (result) {
              case send_tab_to_self::SendTabToSelfResult::kSuccess:
                send_tab_to_self::ShowTabSentSuccessToast(
                    contents.get(), device_name, form_factor);
                break;
              case send_tab_to_self::SendTabToSelfResult::kSuccessThrottled:
                send_tab_to_self::ShowTabSentThrottledToast(
                    contents.get(), device_name, form_factor);
                break;
              default:
                send_tab_to_self::ShowTabSentFailure(contents.get(), result);
                break;
            }
          },
          shown, device.device_name, device.form_factor),
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
                     node.collapsed());
  if (node.type() == SidebarNode::Type::kTab) {
    page_->NodeLiveItemChanged(node.id().AsLowercaseString(),
                               node.is_live_item(), node.unseen());
  } else if (node.type() == SidebarNode::Type::kFolder) {
    page_->LiveFolderChanged(node.id().AsLowercaseString(),
                             ToMojoLiveFolder(node));
  }
}

void SidebarPageHandler::OnNodeWillBeRemoved(const SidebarNode& node) {
  page_->NodeRemoved(node.id().AsLowercaseString());
}

void SidebarPageHandler::OnNodeWillMoveToSpace(const SidebarNode& node) {
  page_->NodeRemoved(node.id().AsLowercaseString());
}

void SidebarPageHandler::OnArchiveChanged(const SpaceModel& space) {
  SendArchive();
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
    const SpaceModel* space = entry.space;
    auto mojo_space = sidebar::mojom::Space::New();
    mojo_space->profile_id =
        entry.profile->GetOriginalProfile()->GetBaseName().AsUTF8Unsafe();
    mojo_space->profile_name =
        base::UTF16ToUTF8(SpaceDirectory::GetProfileName(entry.profile));
    mojo_space->own_profile = entry.profile == own_profile;
    mojo_space->id = space->id().AsLowercaseString();
    mojo_space->name = base::UTF16ToUTF8(space->name());
    mojo_space->color = ToMojoColor(space->color());
    for (SpaceColor color : space->gradient()) {
      mojo_space->gradient.push_back(ToMojoColor(color));
    }
    mojo_space->intensity = space->intensity();
    mojo_space->grain = space->grain();
    mojo_space->appearance =
        static_cast<sidebar::mojom::SpaceAppearance>(space->appearance());
    mojo_space->icon = ToMojoIcon(space->icon());
    mojo_space->archive_after_hours =
        static_cast<uint32_t>(space->archive_after().IntDiv(GetArchiveHour()));
    spaces.push_back(std::move(mojo_space));
  }
  // The profiles a space can be assigned to, as the profile menu lists them.
  std::vector<sidebar::mojom::ProfileOptionPtr> profiles;
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (directory() && profile_manager) {
    for (ProfileAttributesEntry* profile :
         profile_manager->GetProfileAttributesStorage()
             .GetAllProfilesAttributesSortedForDisplay()) {
      if (!profile->IsOmitted()) {
        profiles.push_back(sidebar::mojom::ProfileOption::New(
            profile->GetPath().BaseName().AsUTF8Unsafe(),
            base::UTF16ToUTF8(profile->GetName())));
      }
    }
  }
  page_->SetSpaces(std::move(spaces), space_model().id().AsLowercaseString(),
                   std::move(profiles));
}

void SidebarPageHandler::SendArchive() {
  std::vector<sidebar::mojom::ArchivedEntryPtr> entries;
  for (const ArchivedEntry& entry : space_model().archive()) {
    auto mojo_entry = sidebar::mojom::ArchivedEntry::New();
    mojo_entry->id = entry.id.AsLowercaseString();
    mojo_entry->archived_at = entry.archived_at.InMillisecondsFSinceUnixEpoch();
    for (const ArchivedEntry::Tab& tab : entry.tabs) {
      mojo_entry->tabs.push_back(sidebar::mojom::ArchivedTab::New(
          base::UTF16ToUTF8(tab.title), tab.url));
    }
    entries.push_back(std::move(mojo_entry));
  }
  page_->SetArchive(std::move(entries));
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

void SidebarPageHandler::GetLivePlugins(GetLivePluginsCallback callback) {
  LiveFolderService* service = LiveFolderService::Get(browser_->GetProfile());
  if (!service) {
    std::move(callback).Run({});
    return;
  }
  // Local plugins change on disk: read them again each time.
  service->RescanPlugins(base::BindOnce(
      [](base::WeakPtr<SidebarPageHandler> handler,
         GetLivePluginsCallback callback) {
        std::vector<sidebar::mojom::LivePluginPtr> plugins;
        LiveFolderService* service =
            handler ? LiveFolderService::Get(handler->browser_->GetProfile())
                    : nullptr;
        if (service) {
          for (const LivePlugin& plugin : service->registry().plugins()) {
            plugins.push_back(ToMojoPlugin(plugin, &service->store()));
          }
        }
        std::move(callback).Run(std::move(plugins));
      },
      weak_factory_.GetWeakPtr(), std::move(callback)));
}

void SidebarPageHandler::OpenLivePluginsFolder() {
  if (LiveFolderService* service =
          LiveFolderService::Get(browser_->GetProfile())) {
    service->OpenPluginsFolder();
  }
}

void SidebarPageHandler::GetLiveFolderSource(
    const std::string& id,
    GetLiveFolderSourceCallback callback) {
  SidebarNode* node = FindNode(id);
  LiveFolderService* service = LiveFolderService::Get(browser_->GetProfile());
  if (!node || !node->is_live_folder() || !service) {
    std::move(callback).Run(nullptr);
    return;
  }
  const LiveSource& source = *node->live_source();
  std::move(callback).Run(sidebar::mojom::LiveFolderSource::New(
      source.plugin_id, base::UTF16ToUTF8(node->title()),
      base::WriteJson(source.config).value_or("{}"),
      service->store().CookiesGranted(node->id()),
      source.plugin_url.is_valid() ? source.plugin_url.spec() : std::string(),
      source.plugin_sha256));
}

void SidebarPageHandler::SaveLiveFolder(sidebar::mojom::LiveFolderEditPtr edit,
                                        SaveLiveFolderCallback callback) {
  LiveFolderService* service = LiveFolderService::Get(browser_->GetProfile());
  if (!service) {
    std::move(callback).Run("Live folders aren't available here.");
    return;
  }
  std::optional<base::Value> config =
      base::JSONReader::Read(edit->config_json, base::JSON_PARSE_RFC);
  if (!config || !config->is_dict()) {
    std::move(callback).Run("The settings couldn't be read.");
    return;
  }
  std::map<std::string, std::string> secrets(edit->secrets.begin(),
                                             edit->secrets.end());
  if (edit->folder_id) {
    SidebarNode* folder = FindNode(*edit->folder_id);
    auto result = service->EditLiveFolder(folder, std::move(config->GetDict()),
                                          secrets, edit->grant_cookies);
    std::move(callback).Run(result.has_value() ? std::string()
                                               : result.error());
    return;
  }
  SpaceModel& space = space_model();
  SidebarNode* parent =
      edit->parent_id ? FindNode(*edit->parent_id) : space.pinned_root();
  if (!parent || !space.Contains(parent)) {
    parent = space.pinned_root();
  }
  auto result = service->CreateLiveFolder(
      space, parent, parent->children().size(), base::UTF8ToUTF16(edit->title),
      edit->plugin_id, std::move(config->GetDict()), secrets,
      edit->grant_cookies);
  std::move(callback).Run(result.has_value() ? std::string() : result.error());
}

void SidebarPageHandler::RefreshLiveFolder(const std::string& id) {
  SidebarNode* node = FindNode(id);
  LiveFolderService* service = LiveFolderService::Get(browser_->GetProfile());
  if (node && node->is_live_folder() && service) {
    service->Refresh(node->id());
  }
}

void SidebarPageHandler::PreviewLivePlugin(
    const std::string& url,
    PreviewLivePluginCallback callback) {
  LiveFolderService* service = LiveFolderService::Get(browser_->GetProfile());
  if (!service) {
    std::move(callback).Run(nullptr, "Live folders aren't available here.");
    return;
  }
  service->PreviewPlugin(
      GURL(url),
      base::BindOnce(
          [](PreviewLivePluginCallback callback,
             base::expected<LivePluginPackage, std::string> package) {
            if (!package.has_value()) {
              std::move(callback).Run(nullptr, package.error());
              return;
            }
            const LivePluginManifest& manifest = package->manifest;
            auto preview = sidebar::mojom::LivePluginPreview::New();
            preview->id = manifest.id;
            preview->name = manifest.name;
            preview->description = manifest.description;
            preview->version = manifest.version;
            preview->hosts = manifest.hosts;
            preview->cookie_hosts = manifest.cookies;
            for (const auto& secret : manifest.secrets) {
              preview->secrets.push_back(secret.label);
            }
            preview->source = package->source.spec();
            preview->sha256 = package->sha256;
            std::move(callback).Run(std::move(preview), std::string());
          },
          std::move(callback)));
}

void SidebarPageHandler::InstallLivePlugin(
    const std::string& url,
    const std::string& sha256,
    InstallLivePluginCallback callback) {
  LiveFolderService* service = LiveFolderService::Get(browser_->GetProfile());
  if (!service) {
    std::move(callback).Run("Live folders aren't available here.");
    return;
  }
  service->InstallPlugin(GURL(url), sha256, std::move(callback));
}

void SidebarPageHandler::RemoveLivePlugin(const std::string& id,
                                          RemoveLivePluginCallback callback) {
  LiveFolderService* service = LiveFolderService::Get(browser_->GetProfile());
  if (!service) {
    std::move(callback).Run();
    return;
  }
  service->RemovePlugin(id, std::move(callback));
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
  result->url = node.url();
  // Live in any window: activating it here moves it here.
  result->live = registry.GetTab(&node) != nullptr;
  result->audio = ToMojoAudio(registry.GetAudioState(&node));
  const tabs::TabInterface* tab = registry.GetTab(&node);
  result->claude =
      tab && claude::ClaudeActivity::IsActing(tab->GetContents());
  result->collapsed = node.collapsed();
  result->live_folder = ToMojoLiveFolder(node);
  result->live_item = node.is_live_item();
  result->unseen = node.unseen();
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

}  // namespace compass
