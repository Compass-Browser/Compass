// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/webui/sidebar/sidebar_page_handler.h"

#include <utility>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/uuid.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "strata/browser/ui/views/command_bar/command_bar.h"
#include "strata/browser/ui/views/sidebar/sidebar_context_menu.h"
#include "strata/browser/ui/views/sidebar/sidebar_hover_card.h"
#include "strata/browser/window_sync/space_directory.h"
#include "strata/browser/window_sync/space_service.h"
#include "strata/browser/window_sync/window_features.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/models/menu_separator_types.h"
#include "ui/gfx/geometry/point.h"

namespace strata {

namespace {

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

// The handler casts between the two.
static_assert(static_cast<int>(sidebar::mojom::SpaceColor::kMaxValue) ==
              static_cast<int>(SpaceColor::kMaxValue));

// A node's context menu. Commands go to the handler by the node's ID, so a
// node that's gone by then is left alone.
class NodeMenu : public ui::SimpleMenuModel,
                 public ui::SimpleMenuModel::Delegate {
 public:
  NodeMenu(base::WeakPtr<SidebarPageHandler> handler, std::string id)
      : ui::SimpleMenuModel(this),
        handler_(std::move(handler)),
        id_(std::move(id)),
        spaces_menu_(this) {}
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

  // ui::SimpleMenuModel::Delegate:
  void ExecuteCommand(int command_id, int event_flags) override {
    if (!handler_) {
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
  if (SpaceDirectory::IsListed(browser_->GetProfile())) {
    if (SpaceDirectory* directory = SpaceDirectory::Get()) {
      uses_directory_ = true;
      directory->AddObserver(this);
    }
  }
  features_->AddObserver(this);
  registry().AddObserver(this);
  tab_strip_model()->AddObserver(this);
}

SidebarPageHandler::~SidebarPageHandler() {
  // A swipe the page was in the middle of.
  if (features_->preview_space()) {
    features_->SetSpacePreview(nullptr, 0);
  }
  registry().RemoveObserver(this);
  features_->RemoveObserver(this);
  if (SpaceDirectory* directory = this->directory()) {
    directory->RemoveObserver(this);
  }
  features_->service().space_list().RemoveObserver(this);
  favorites().RemoveObserver(this);
  observed_space_->RemoveObserver(this);
}

void SidebarPageHandler::RequestTree() {
  SendSpaces();
  auto tree = sidebar::mojom::Tree::New();
  tree->favorites = ToMojo(*favorites().pinned_root());
  tree->pinned = ToMojo(*space_model().pinned_root());
  tree->today = ToMojo(*space_model().today_root());
  tree->active_id = GetActiveNodeId();
  page_->SetTree(std::move(tree));
  SendArchive();
}

void SidebarPageHandler::ActivateNode(const std::string& id) {
  SidebarNode* node = FindNode(id);
  if (!node || node->type() != SidebarNode::Type::kTab) {
    return;
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
  switch (node->type()) {
    case SidebarNode::Type::kTab:
      CloseTabNode(node);
      break;
    case SidebarNode::Type::kSplit: {
      // Closing a pane can dissolve the split, so go by ID.
      std::vector<base::Uuid> panes;
      for (const auto& pane : node->children()) {
        panes.push_back(pane->id());
      }
      for (const base::Uuid& pane_id : panes) {
        if (SidebarNode* pane = space_model().FindById(pane_id)) {
          CloseTabNode(pane);
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

void SidebarPageHandler::NewTab() {
  if (CommandBar* bar = CommandBar::From(browser_)) {
    bar->Show(command_bar::mojom::Mode::kNewEntry);
    return;
  }
  // No views (unit tests).
  chrome::NewTab(browser_, NewTabTypes::kNewTabButton);
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
      !space_model().pinned_root()->Contains(parent) ||
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

void SidebarPageHandler::DeleteNode(const std::string& id) {
  SidebarNode* node = FindNode(id);
  if (!node || node->type() == SidebarNode::Type::kRoot) {
    return;
  }
  if (space_model().pinned_root()->Contains(node) ||
      favorites().Contains(node)) {
    GetModelOf(node).Remove(node);
  }
}

void SidebarPageHandler::SwitchSpace(const std::string& id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  if (entry.space) {
    // Another profile's swaps the window, which closes with this handler.
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

void SidebarPageHandler::SetSpaceColor(const std::string& id,
                                       sidebar::mojom::SpaceColor color) {
  // Mojo has validated |color|.
  if (SpaceModel* space = FindAnySpace(id).space) {
    space->SetColor(static_cast<SpaceColor>(color));
  }
}

void SidebarPageHandler::DeleteSpace(const std::string& id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  if (entry.space) {
    strata::DeleteSpace(entry.profile, *entry.space);
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
    space->SetArchiveAfter(base::Hours(hours));
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
    menu->AddItem(kNewFolder, u"New Folder");
    menu->AddItem(kRename, u"Rename");
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
  if (node->type() == SidebarNode::Type::kTab) {
    menu->AddItem(kCopyLink, u"Copy Link");
    if (entry) {
      menu->AddItem(kDuplicate, u"Duplicate");
    }
    if (live) {
      menu->AddItem(kReload, u"Reload");
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
          .WriteText(base::UTF8ToUTF16(node->url().spec()));
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
      if (node->type() == SidebarNode::Type::kFolder) {
        page_->StartRename(id);
      }
      break;
    case kCloseTab:
      CloseNode(id);
      break;
    case kRemove:
      DeleteNode(id);
      break;
  }
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
}

void SidebarPageHandler::OnNodeChanged(const SidebarNode& node) {
  page_->NodeChanged(node.id().AsLowercaseString(),
                     base::UTF16ToUTF8(node.title()), node.url());
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

void SidebarPageHandler::OnSpaceSwitched() {
  observed_space_->RemoveObserver(this);
  observed_space_ = &space_model();
  observed_space_->AddObserver(this);
  RequestTree();
}

void SidebarPageHandler::OnSwappedAway() {
  // The window has no bridge left to act through, and closes in a moment.
  receiver_.reset();
}

void SidebarPageHandler::OnNodeLiveChanged(const SidebarNode& node, bool live) {
  page_->NodeLiveChanged(node.id().AsLowercaseString(), live);
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
    mojo_space->profile_name =
        base::UTF16ToUTF8(SpaceDirectory::GetProfileName(entry.profile));
    mojo_space->own_profile = entry.profile == own_profile;
    mojo_space->id = space->id().AsLowercaseString();
    mojo_space->name = base::UTF16ToUTF8(space->name());
    mojo_space->color = static_cast<sidebar::mojom::SpaceColor>(space->color());
    mojo_space->archive_after_hours = space->archive_after().InHours();
    spaces.push_back(std::move(mojo_space));
  }
  page_->SetSpaces(std::move(spaces),
                   space_model().id().AsLowercaseString());
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
  result->url = node.url();
  // Live in any window: activating it here moves it here.
  result->live = registry.GetTab(&node) != nullptr;
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

void SidebarPageHandler::CloseTabNode(SidebarNode* tab_node) {
  tabs::TabInterface* tab = bridge().GetTab(tab_node);
  if (!tab) {
    GetModelOf(tab_node).Remove(tab_node);
    return;
  }
  tab_strip_model()->CloseWebContentsAt(
      tab_strip_model()->GetIndexOfTab(tab),
      CLOSE_USER_GESTURE | CLOSE_CREATE_HISTORICAL_TAB);
}

}  // namespace strata
