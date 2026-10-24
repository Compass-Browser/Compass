// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/tab_strip_bridge.h"

#include <memory>
#include <vector>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/tab_list/tab_removed_reason.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_contents.h"
#include "strata/browser/window_sync/live_tab_registry.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"

namespace strata {

namespace {

// Appends the live tabs under |node| to |tabs|, depth first.
void CollectLiveTabs(const TabStripBridge& bridge,
                     const SidebarNode* node,
                     std::vector<tabs::TabInterface*>& tabs) {
  if (tabs::TabInterface* tab = bridge.GetTab(node)) {
    tabs.push_back(tab);
  }
  for (const auto& child : node->children()) {
    CollectLiveTabs(bridge, child.get(), tabs);
  }
}

// Counts the live tabs under |node| that come before |target|, depth first.
// Returns true once |target| is reached.
bool CountLiveTabsBefore(const TabStripBridge& bridge,
                         const SidebarNode* node,
                         const SidebarNode* target,
                         int& count) {
  if (node == target) {
    return true;
  }
  if (bridge.GetTab(node)) {
    ++count;
  }
  for (const auto& child : node->children()) {
    if (CountLiveTabsBefore(bridge, child.get(), target, count)) {
      return true;
    }
  }
  return false;
}

}  // namespace

TabStripBridge::TabStripBridge(BrowserWindowInterface* browser,
                               SpaceModel* space_model,
                               LiveTabRegistry* registry)
    : browser_(browser),
      tab_strip_model_(browser->GetTabStripModel()),
      space_model_(space_model),
      registry_(registry) {
  CHECK(tab_strip_model_);
  CHECK(space_model_);
  CHECK(registry_);
  for (int i = 0; i < tab_strip_model_->count(); ++i) {
    AddNodeForTab(tab_strip_model_->GetTabAtIndex(i), i);
  }
  tab_strip_model_->AddObserver(this);
  space_model_->AddObserver(this);
}

TabStripBridge::~TabStripBridge() {
  space_model_->RemoveObserver(this);
  if (tab_strip_model_) {
    tab_strip_model_->RemoveObserver(this);
  }
}

tabs::TabInterface* TabStripBridge::GetTab(const SidebarNode* node) const {
  tabs::TabInterface* tab = registry_->GetTab(node);
  return tab && tab->GetBrowserWindowInterface() == browser_ ? tab : nullptr;
}

SidebarNode* TabStripBridge::GetNode(const tabs::TabInterface* tab) const {
  return registry_->GetNode(tab);
}

tabs::TabInterface* TabStripBridge::OpenNode(SidebarNode* tab_node) {
  CHECK_EQ(tab_node->type(), SidebarNode::Type::kTab);
  if (tabs::TabInterface* tab = GetTab(tab_node)) {
    return tab;
  }
  if (!tab_strip_model_) {
    return nullptr;
  }
  // The node's place in the strip: the live tabs before it, depth first.
  int index = 0;
  if (!CountLiveTabsBefore(*this, space_model_->pinned_root(), tab_node,
                           index)) {
    CountLiveTabsBefore(*this, space_model_->today_root(), tab_node, index);
  }

  // Live in another window: move it here, the way tab dragging does. That
  // window's bridge keeps the node, and AddNodeForTab() finds it.
  if (tabs::TabInterface* tab = registry_->GetTab(tab_node)) {
    BrowserWindowInterface* source = tab->GetBrowserWindowInterface();
    if (!source) {
      return nullptr;
    }
    TabStripModel* source_strip = source->GetTabStripModel();
    std::unique_ptr<tabs::TabModel> detached =
        source_strip->DetachTabAtForInsertion(source_strip->GetIndexOfTab(tab));
    tab_strip_model_->InsertDetachedTabAt(index, std::move(detached),
                                          AddTabTypes::ADD_ACTIVE);
    return GetTab(tab_node);
  }

  const GURL url = tab_node->url().is_valid()
                       ? tab_node->url()
                       : GURL(chrome::kChromeUINewTabURL);
  NavigateParams params(browser_, url, ui::PAGE_TRANSITION_AUTO_BOOKMARK);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  params.tabstrip_index = index;
  base::AutoReset<raw_ptr<SidebarNode>> adopting(&adopting_node_, tab_node);
  Navigate(&params);
  return GetTab(tab_node);
}

void TabStripBridge::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  if (updating_) {
    return;
  }
  switch (change.type()) {
    case TabStripModelChange::kInserted:
      for (const auto& inserted : change.GetInsert()->contents) {
        AddNodeForTab(inserted.tab, inserted.index);
      }
      break;
    case TabStripModelChange::kRemoved:
      for (const auto& removed : change.GetRemove()->contents) {
        RemoveNodeForTab(removed.tab, TabRemoveReasonUtils::WillDeleteTab(
                                          removed.remove_reason));
      }
      break;
    case TabStripModelChange::kMoved:
      MoveNodeForTab(change.GetMove()->tab, change.GetMove()->to_index);
      break;
    case TabStripModelChange::kReplaced:
      UpdateNode(change.GetReplace()->tab);
      break;
    case TabStripModelChange::kSelectionOnly:
      break;
  }
}

void TabStripBridge::OnTabChangedAt(tabs::TabInterface* tab,
                                    TabChangeType change_type) {
  // Titles and URLs arrive with kAll; kLoadingOnly fires every frame.
  if (change_type == TabChangeType::kAll) {
    UpdateNode(tab);
  }
}

void TabStripBridge::OnTabStripModelDestroyed(TabStripModel* tab_strip_model) {
  tab_strip_model_ = nullptr;
}

void TabStripBridge::OnNodeMoved(const SidebarNode& node,
                                 const SidebarNode& old_parent,
                                 size_t old_index) {
  if (!updating_) {
    ScheduleSyncStripOrder();
  }
}

void TabStripBridge::OnNodeWillBeRemoved(const SidebarNode& node) {
  std::vector<tabs::TabInterface*> live_tabs;
  CollectLiveTabs(*this, &node, live_tabs);
  std::vector<tabs::TabHandle> to_close;
  for (tabs::TabInterface* tab : live_tabs) {
    registry_->Unregister(tab);
    to_close.push_back(tab->GetHandle());
  }
  if (updating_ || to_close.empty()) {
    return;
  }
  // The model may be changed while the strip notifies, so close in a task.
  // Unload handlers can still keep a tab open; it then has no node.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<TabStripBridge> bridge,
             std::vector<tabs::TabHandle> handles) {
            if (!bridge || !bridge->tab_strip_model_) {
              return;
            }
            for (const tabs::TabHandle& handle : handles) {
              const int index =
                  bridge->tab_strip_model_->GetIndexOfTab(handle.Get());
              if (index != TabStripModel::kNoTab) {
                bridge->tab_strip_model_->CloseWebContentsAt(
                    index, CLOSE_CREATE_HISTORICAL_TAB);
              }
            }
          },
          weak_factory_.GetWeakPtr(), std::move(to_close)));
}

void TabStripBridge::AddNodeForTab(tabs::TabInterface* tab, int index) {
  // A tab may land before the pinned block (or between a split's panes);
  // put it back in the tree's order afterwards.
  ScheduleSyncStripOrder();
  if (GetNode(tab)) {
    // Moved here from another window, with its node.
    return;
  }
  if (adopting_node_) {
    registry_->Register(tab, adopting_node_);
    adopting_node_ = nullptr;
    return;
  }
  content::WebContents* contents = tab->GetContents();
  base::AutoReset<bool> updating(&updating_, true);
  SidebarNode* node = space_model_->AddTab(
      space_model_->today_root(), GetTodayIndexAfter(index, nullptr),
      contents->GetLastCommittedURL(), contents->GetTitle());
  registry_->Register(tab, node);
}

void TabStripBridge::MoveNodeForTab(tabs::TabInterface* tab, int to_index) {
  SidebarNode* node = GetNode(tab);
  if (!node) {
    return;
  }
  SidebarNode* today = space_model_->today_root();
  if (node->parent() != today) {
    // Splits (and later pinned entries) keep their place; undo the move.
    ScheduleSyncStripOrder();
    return;
  }
  {
    base::AutoReset<bool> updating(&updating_, true);
    space_model_->Move(node, today, GetTodayIndexAfter(to_index, node));
  }
  // The strip may have put the tab before the pinned block.
  ScheduleSyncStripOrder();
}

void TabStripBridge::RemoveNodeForTab(tabs::TabInterface* tab,
                                      bool will_delete) {
  SidebarNode* node = GetNode(tab);
  if (!node || !will_delete) {
    return;
  }
  registry_->Unregister(tab);
  if (IsPinned(node)) {
    return;
  }
  base::AutoReset<bool> updating(&updating_, true);
  space_model_->Remove(node);
}

bool TabStripBridge::IsPinned(const SidebarNode* node) const {
  return space_model_->pinned_root()->Contains(node);
}

void TabStripBridge::UpdateNode(tabs::TabInterface* tab) {
  SidebarNode* node = GetNode(tab);
  if (!node) {
    return;
  }
  content::WebContents* contents = tab->GetContents();
  if (node->url() != contents->GetLastCommittedURL()) {
    space_model_->SetUrl(node, contents->GetLastCommittedURL());
  }
  if (node->title() != contents->GetTitle()) {
    space_model_->SetTitle(node, contents->GetTitle());
  }
}

SidebarNode* TabStripBridge::GetTodayEntryAt(int index) const {
  if (index < 0 || index >= tab_strip_model_->count()) {
    return nullptr;
  }
  SidebarNode* node = GetNode(tab_strip_model_->GetTabAtIndex(index));
  const SidebarNode* today = space_model_->today_root();
  while (node && node->parent() != today) {
    node = node->parent();
  }
  return node;
}

size_t TabStripBridge::GetTodayIndexAfter(int index,
                                          const SidebarNode* moving) const {
  const SidebarNode* today = space_model_->today_root();
  const SidebarNode* previous = GetTodayEntryAt(index - 1);
  if (!previous || previous == moving) {
    return 0;
  }
  size_t result = today->GetIndexOf(previous) + 1;
  if (moving && moving->parent() == today &&
      today->GetIndexOf(moving) < result) {
    --result;
  }
  return result;
}

void TabStripBridge::SyncStripOrder() {
  if (!tab_strip_model_) {
    return;
  }
  std::vector<tabs::TabInterface*> order;
  CollectLiveTabs(*this, space_model_->pinned_root(), order);
  CollectLiveTabs(*this, space_model_->today_root(), order);

  base::AutoReset<bool> updating(&updating_, true);
  for (size_t i = 0; i < order.size(); ++i) {
    const int from = tab_strip_model_->GetIndexOfTab(order[i]);
    if (from != static_cast<int>(i)) {
      tab_strip_model_->MoveWebContentsAt(from, i,
                                          /*select_after_move=*/false);
    }
  }
}

void TabStripBridge::ScheduleSyncStripOrder() {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&TabStripBridge::SyncStripOrder,
                                weak_factory_.GetWeakPtr()));
}

}  // namespace strata
