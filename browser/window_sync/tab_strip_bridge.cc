// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/tab_strip_bridge.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/base64.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/tab_list/tab_removed_reason.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/webui_url_constants.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "strata/browser/window_sync/live_tab_registry.h"
#include "strata/browser/window_sync/window_features.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace strata {

namespace {

// How long a tab that's leaving may take to hand over its last frame.
constexpr base::TimeDelta kSnapshotTimeout = base::Seconds(1);

// JPEG keeps a full-window screenshot small enough to pass to the page.
constexpr int kSnapshotJpegQuality = 80;

std::string EncodeSnapshot(const SkBitmap& bitmap) {
  std::optional<std::vector<uint8_t>> jpeg =
      gfx::JPEGCodec::Encode(bitmap, kSnapshotJpegQuality);
  if (!jpeg) {
    return std::string();
  }
  return "data:image/jpeg;base64," + base::Base64Encode(*jpeg);
}

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
  did_become_active_subscription_ =
      browser_->RegisterDidBecomeActive(base::BindRepeating(
          &TabStripBridge::OnWindowBecameActive, base::Unretained(this)));
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
    // Leave the other window on its previous tab, or a placeholder.
    if (WindowFeatures* source_features = WindowFeatures::From(source)) {
      source_features->tab_strip_bridge().PrepareToLoseTab(tab);
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

SidebarNode* TabStripBridge::GetPlaceholderNode(
    const tabs::TabInterface* tab) const {
  if (!tab) {
    return nullptr;
  }
  auto it = placeholders_.find(tab->GetHandle());
  return it == placeholders_.end() ? nullptr
                                   : space_model_->FindById(it->second);
}

SidebarNode* TabStripBridge::GetShownNode(const tabs::TabInterface* tab) const {
  SidebarNode* node = GetNode(tab);
  return node ? node : GetPlaceholderNode(tab);
}

void TabStripBridge::PrepareToLoseTab(tabs::TabInterface* tab) {
  if (!tab_strip_model_ || tab_strip_model_->GetActiveTab() != tab) {
    return;
  }
  for (const tabs::TabHandle& handle : mru_) {
    tabs::TabInterface* previous = handle.Get();
    if (!previous || previous == tab || IsPlaceholder(previous)) {
      continue;
    }
    const int index = tab_strip_model_->GetIndexOfTab(previous);
    if (index != TabStripModel::kNoTab) {
      tab_strip_model_->ActivateTabAt(index);
      return;
    }
  }
  if (SidebarNode* node = GetNode(tab)) {
    CaptureSnapshot(tab, node->id());
    InsertPlaceholder(node);
  }
}

void TabStripBridge::GetSnapshot(
    const tabs::TabInterface* placeholder,
    base::OnceCallback<void(const std::string&)> callback) {
  auto placeholder_it = placeholder
                            ? placeholders_.find(placeholder->GetHandle())
                            : placeholders_.end();
  if (placeholder_it == placeholders_.end()) {
    std::move(callback).Run(std::string());
    return;
  }
  auto it = snapshots_.find(placeholder_it->second);
  if (it == snapshots_.end()) {
    std::move(callback).Run(std::string());
  } else if (it->second.data_url) {
    std::move(callback).Run(*it->second.data_url);
  } else {
    it->second.waiters.push_back(std::move(callback));
  }
}

void TabStripBridge::HandOffTabsTo(TabStripBridge& target) {
  CHECK(tab_strip_model_);
  CHECK(target.tab_strip_model_);
  // One pass, front to back, so the target sees them in strip order. Closing
  // a placeholder is synchronous (it has no unload handlers); the bound only
  // guards against one that isn't.
  for (int pass = tab_strip_model_->count();
       pass > 0 && !tab_strip_model_->empty(); --pass) {
    tabs::TabInterface* tab = tab_strip_model_->GetTabAtIndex(0);
    if (IsPlaceholder(tab)) {
      tab_strip_model_->CloseWebContentsAt(0, CLOSE_NONE);
      continue;
    }
    std::unique_ptr<tabs::TabModel> detached =
        tab_strip_model_->DetachTabAtForInsertion(0);
    target.tab_strip_model_->InsertDetachedTabAt(
        target.tab_strip_model_->count(), std::move(detached),
        AddTabTypes::ADD_NONE);
  }
}

void TabStripBridge::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  if (selection.active_tab_changed() && selection.new_tab) {
    MarkUsed(selection.new_tab);
    // Using any other tab dismisses this window's placeholders.
    if (!placeholders_.empty() && !IsPlaceholder(selection.new_tab)) {
      ScheduleClosePlaceholders();
    }
  }
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
  if (!placeholders_.empty()) {
    // Placeholders for this subtree have nothing left to bring back.
    ScheduleClosePlaceholders();
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
  if (placeholder_node_) {
    placeholders_[tab->GetHandle()] = placeholder_node_->id();
    placeholder_node_ = nullptr;
    return;
  }
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
  auto placeholder = placeholders_.find(tab->GetHandle());
  if (placeholder != placeholders_.end()) {
    const base::Uuid node_id = placeholder->second;
    placeholders_.erase(placeholder);
    DropSnapshot(node_id);
    return;
  }
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

TabStripBridge::Snapshot::Snapshot() = default;
TabStripBridge::Snapshot::Snapshot(Snapshot&&) = default;
TabStripBridge::Snapshot& TabStripBridge::Snapshot::operator=(Snapshot&&) =
    default;
TabStripBridge::Snapshot::~Snapshot() = default;

void TabStripBridge::CaptureSnapshot(tabs::TabInterface* tab,
                                     const base::Uuid& node_id) {
  DropSnapshot(node_id);
  snapshots_[node_id];
  content::RenderWidgetHostView* view =
      tab->GetContents()->GetRenderWidgetHostView();
  if (!view) {
    OnSnapshotReady(node_id, std::string());
    return;
  }
  // The request goes out now, for the frame the tab shows here; the result
  // comes back after the tab has moved.
  view->CopyFromSurface(
      gfx::Rect(), gfx::Size(), kSnapshotTimeout,
      base::BindOnce(
          [](base::WeakPtr<TabStripBridge> bridge, base::Uuid node_id,
             const content::CopyFromSurfaceResult& result) {
            if (!bridge) {
              return;
            }
            if (!result.has_value() || result->bitmap.drawsNothing()) {
              bridge->OnSnapshotReady(node_id, std::string());
              return;
            }
            base::ThreadPool::PostTaskAndReplyWithResult(
                FROM_HERE, {base::TaskPriority::USER_VISIBLE},
                base::BindOnce(&EncodeSnapshot, result->bitmap),
                base::BindOnce(&TabStripBridge::OnSnapshotReady, bridge,
                               node_id));
          },
          weak_factory_.GetWeakPtr(), node_id));
}

void TabStripBridge::OnSnapshotReady(const base::Uuid& node_id,
                                     std::string data_url) {
  auto it = snapshots_.find(node_id);
  if (it == snapshots_.end()) {
    // Its placeholder closed in the meantime.
    return;
  }
  it->second.data_url = std::move(data_url);
  for (auto& waiter : std::exchange(it->second.waiters, {})) {
    std::move(waiter).Run(*it->second.data_url);
  }
}

void TabStripBridge::DropSnapshot(const base::Uuid& node_id) {
  auto it = snapshots_.find(node_id);
  if (it == snapshots_.end()) {
    return;
  }
  std::vector<base::OnceCallback<void(const std::string&)>> waiters =
      std::move(it->second.waiters);
  snapshots_.erase(it);
  for (auto& waiter : waiters) {
    std::move(waiter).Run(std::string());
  }
}

void TabStripBridge::OnWindowBecameActive(BrowserWindowInterface* browser) {
  if (tab_strip_model_ &&
      GetPlaceholderNode(tab_strip_model_->GetActiveTab())) {
    // Not from inside the activation notification.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&TabStripBridge::BringBackShownPlaceholder,
                                  weak_factory_.GetWeakPtr()));
  }
}

void TabStripBridge::BringBackShownPlaceholder() {
  if (!tab_strip_model_ || !browser_->IsActive()) {
    return;
  }
  SidebarNode* node = GetPlaceholderNode(tab_strip_model_->GetActiveTab());
  if (!node) {
    return;
  }
  if (tabs::TabInterface* tab = OpenNode(node)) {
    tab_strip_model_->ActivateTabAt(tab_strip_model_->GetIndexOfTab(tab));
  }
}

bool TabStripBridge::IsPlaceholder(const tabs::TabInterface* tab) const {
  return tab && placeholders_.contains(tab->GetHandle());
}

void TabStripBridge::InsertPlaceholder(SidebarNode* node) {
  if (!node || !tab_strip_model_) {
    return;
  }
  NavigateParams params(browser_, GURL(kPlaceholderURL),
                        ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  params.tabstrip_index = tab_strip_model_->count();
  base::AutoReset<raw_ptr<SidebarNode>> placeholder(&placeholder_node_, node);
  Navigate(&params);
}

void TabStripBridge::ClosePlaceholders() {
  if (!tab_strip_model_) {
    return;
  }
  std::vector<tabs::TabHandle> to_close;
  for (const auto& [handle, node_id] : placeholders_) {
    tabs::TabInterface* tab = handle.Get();
    if (tab && (tab != tab_strip_model_->GetActiveTab() ||
                !space_model_->FindById(node_id))) {
      to_close.push_back(handle);
    }
  }
  for (const tabs::TabHandle& handle : to_close) {
    const int index = tab_strip_model_->GetIndexOfTab(handle.Get());
    if (index != TabStripModel::kNoTab) {
      tab_strip_model_->CloseWebContentsAt(index, CLOSE_NONE);
    }
  }
}

void TabStripBridge::ScheduleClosePlaceholders() {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&TabStripBridge::ClosePlaceholders,
                                weak_factory_.GetWeakPtr()));
}

void TabStripBridge::MarkUsed(tabs::TabInterface* tab) {
  std::erase(mru_, tab->GetHandle());
  mru_.insert(mru_.begin(), tab->GetHandle());
  // Drop handles of tabs that are gone, so the list stays short.
  std::erase_if(mru_, [](const tabs::TabHandle& h) { return !h.Get(); });
}

void TabStripBridge::ScheduleSyncStripOrder() {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&TabStripBridge::SyncStripOrder,
                                weak_factory_.GetWeakPtr()));
}

}  // namespace strata
