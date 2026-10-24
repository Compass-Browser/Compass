// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/tab_strip_bridge.h"

#include <vector>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/web_contents.h"

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

}  // namespace

TabStripBridge::TabStripBridge(TabStripModel* tab_strip_model,
                               SpaceModel* space_model)
    : tab_strip_model_(tab_strip_model), space_model_(space_model) {
  CHECK(tab_strip_model_);
  CHECK(space_model_);
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
  auto it = tab_for_node_.find(node);
  return it == tab_for_node_.end() ? nullptr : it->second.Get();
}

SidebarNode* TabStripBridge::GetNode(const tabs::TabInterface* tab) const {
  if (!tab) {
    return nullptr;
  }
  auto it = node_for_tab_.find(tab->GetHandle());
  return it == node_for_tab_.end() ? nullptr : it->second.get();
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
        RemoveNodeForTab(removed.tab);
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
  node_for_tab_.clear();
  tab_for_node_.clear();
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
    tab_for_node_.erase(GetNode(tab));
    node_for_tab_.erase(tab->GetHandle());
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
  content::WebContents* contents = tab->GetContents();
  base::AutoReset<bool> updating(&updating_, true);
  SidebarNode* node = space_model_->AddTab(
      space_model_->today_root(), GetTodayIndexAfter(index, nullptr),
      contents->GetLastCommittedURL(), contents->GetTitle());
  node_for_tab_[tab->GetHandle()] = node;
  tab_for_node_[node] = tab->GetHandle();
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
  base::AutoReset<bool> updating(&updating_, true);
  space_model_->Move(node, today, GetTodayIndexAfter(to_index, node));
}

void TabStripBridge::RemoveNodeForTab(tabs::TabInterface* tab) {
  SidebarNode* node = GetNode(tab);
  if (!node) {
    return;
  }
  base::AutoReset<bool> updating(&updating_, true);
  // OnNodeWillBeRemoved() drops the mapping.
  space_model_->Remove(node);
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
