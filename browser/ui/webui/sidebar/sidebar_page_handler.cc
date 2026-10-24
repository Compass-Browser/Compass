// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/webui/sidebar/sidebar_page_handler.h"

#include <utility>
#include <vector>

#include "base/check.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
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

}  // namespace

SidebarPageHandler::SidebarPageHandler(
    mojo::PendingReceiver<sidebar::mojom::PageHandler> receiver,
    mojo::PendingRemote<sidebar::mojom::Page> page,
    BrowserWindowInterface* browser)
    : browser_(browser),
      features_(WindowFeatures::From(browser)),
      receiver_(this, std::move(receiver)),
      page_(std::move(page)) {
  CHECK(features_);
  space_model().AddObserver(this);
  registry().AddObserver(this);
  tab_strip_model()->AddObserver(this);
}

SidebarPageHandler::~SidebarPageHandler() {
  registry().RemoveObserver(this);
  space_model().RemoveObserver(this);
}

void SidebarPageHandler::RequestTree() {
  auto tree = sidebar::mojom::Tree::New();
  tree->pinned = ToMojo(*space_model().pinned_root());
  tree->today = ToMojo(*space_model().today_root());
  tree->active_id = GetActiveNodeId();
  page_->SetTree(std::move(tree));
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

void SidebarPageHandler::OnNodeLiveChanged(const SidebarNode& node, bool live) {
  page_->NodeLiveChanged(node.id().AsLowercaseString(), live);
}

void SidebarPageHandler::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  // The bridge observed the strip first, so a new tab already has its node.
  if (selection.active_tab_changed()) {
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

std::optional<std::string> SidebarPageHandler::GetActiveNodeId() {
  const SidebarNode* node = bridge().GetNode(tab_strip_model()->GetActiveTab());
  if (!node) {
    return std::nullopt;
  }
  return node->id().AsLowercaseString();
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
