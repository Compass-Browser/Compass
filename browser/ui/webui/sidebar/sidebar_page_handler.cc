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
#include "strata/browser/ui/views/command_bar/command_bar.h"
#include "strata/browser/ui/views/sidebar/sidebar_hover_card.h"
#include "strata/browser/window_sync/window_features.h"

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
  features_->service().space_list().AddObserver(this);
  features_->AddObserver(this);
  registry().AddObserver(this);
  tab_strip_model()->AddObserver(this);
}

SidebarPageHandler::~SidebarPageHandler() {
  registry().RemoveObserver(this);
  features_->RemoveObserver(this);
  features_->service().space_list().RemoveObserver(this);
  observed_space_->RemoveObserver(this);
}

void SidebarPageHandler::RequestTree() {
  SendSpaces();
  auto tree = sidebar::mojom::Tree::New();
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
  if (!space_model().CanMove(node, new_parent) ||
      index > new_parent->children().size()) {
    return;
  }
  // SpaceModel::Move() counts the index after the node has left its parent.
  if (node->parent() == new_parent && new_parent->GetIndexOf(node) < index) {
    --index;
  }
  space_model().Move(node, new_parent, index);
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
  SidebarNode* node = space_model().AddTab(parent, index, url,
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
  if (node && node->type() != SidebarNode::Type::kRoot &&
      space_model().pinned_root()->Contains(node)) {
    space_model().Remove(node);
  }
}

void SidebarPageHandler::SwitchSpace(const std::string& id) {
  if (SpaceModel* space = FindSpace(id)) {
    features_->SwitchToSpace(*space);
  }
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
  SpaceModel* space = FindSpace(id);
  std::u16string name16;
  base::TrimWhitespace(base::UTF8ToUTF16(name), base::TRIM_ALL, &name16);
  if (space && !name16.empty()) {
    space->SetName(std::move(name16));
  }
}

void SidebarPageHandler::SetSpaceColor(const std::string& id,
                                       sidebar::mojom::SpaceColor color) {
  // Mojo has validated |color|.
  if (SpaceModel* space = FindSpace(id)) {
    space->SetColor(static_cast<SpaceColor>(color));
  }
}

void SidebarPageHandler::DeleteSpace(const std::string& id) {
  if (SpaceModel* space = FindSpace(id)) {
    strata::DeleteSpace(browser_->GetProfile(), *space);
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
  SpaceModel* space = FindSpace(space_id);
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
  SendSpaces();
}

void SidebarPageHandler::OnSpaceUpdated(const SpaceModel& space) {
  SendSpaces();
}

void SidebarPageHandler::OnSpaceWillBeRemoved(SpaceModel& space) {
  // Windows showing it have switched away; the list still has it.
  CHECK_NE(&space, observed_space_);
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&SidebarPageHandler::SendSpaces,
                                weak_factory_.GetWeakPtr()));
}

void SidebarPageHandler::OnSpaceSwitched() {
  observed_space_->RemoveObserver(this);
  observed_space_ = &space_model();
  observed_space_->AddObserver(this);
  RequestTree();
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

TabStripBridge& SidebarPageHandler::bridge() {
  return features_->tab_strip_bridge();
}

LiveTabRegistry& SidebarPageHandler::registry() {
  return features_->registry();
}

void SidebarPageHandler::SendSpaces() {
  std::vector<sidebar::mojom::SpacePtr> spaces;
  for (const auto& space : features_->service().space_list().spaces()) {
    auto mojo_space = sidebar::mojom::Space::New();
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
  auto result = sidebar::mojom::Node::New();
  result->id = node.id().AsLowercaseString();
  result->type = ToMojoType(node.type());
  result->title = base::UTF16ToUTF8(node.title());
  result->url = node.url();
  // Live in any window: activating it here moves it here.
  result->live = registry().GetTab(&node) != nullptr;
  for (const auto& child : node.children()) {
    result->children.push_back(ToMojo(*child));
  }
  return result;
}

SidebarNode* SidebarPageHandler::FindNode(const std::string& id) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  return uuid.is_valid() ? space_model().FindById(uuid) : nullptr;
}

SpaceModel* SidebarPageHandler::FindSpace(const std::string& id) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  return uuid.is_valid() ? features_->service().space_list().FindSpace(uuid)
                         : nullptr;
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

SidebarHoverCard* SidebarPageHandler::hover_card() {
  return page_contents_ ? SidebarHoverCard::FromWebContents(page_contents_)
                        : nullptr;
}

void SidebarPageHandler::CloseTabNode(SidebarNode* tab_node) {
  tabs::TabInterface* tab = bridge().GetTab(tab_node);
  if (!tab) {
    space_model().Remove(tab_node);
    return;
  }
  tab_strip_model()->CloseWebContentsAt(
      tab_strip_model()->GetIndexOfTab(tab),
      CLOSE_USER_GESTURE | CLOSE_CREATE_HISTORICAL_TAB);
}

}  // namespace strata
