// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/tab_strip_bridge.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/base64.h"
#include "base/check.h"
#include "base/strings/utf_string_conversions.h"
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
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "strata/browser/sidebar/sidebar_serializer.h"
#include "strata/browser/window_sync/live_tab_registry.h"
#include "strata/browser/window_sync/restored_tab_matcher.h"
#include "strata/browser/window_sync/window_features.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace strata {

namespace {

// A link folder is named after the page its links came from.
std::u16string GetLinkFolderTitle(const SidebarNode& opener) {
  const SidebarNode* tab = opener.type() == SidebarNode::Type::kSplit &&
                                   !opener.children().empty()
                               ? opener.children()[0].get()
                               : &opener;
  if (!tab->display_title().empty()) {
    return tab->display_title();
  }
  return base::UTF8ToUTF16(tab->url().host());
}

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

// The page |contents| shows. A restored tab that hasn't loaded has no
// document yet, only its restored history.
GURL GetTabURL(content::WebContents* contents) {
  const GURL& url = contents->GetLastCommittedURL();
  if (!url.is_empty()) {
    return url;
  }
  content::NavigationEntry* entry =
      contents->GetController().GetLastCommittedEntry();
  return entry && !entry->IsInitialEntry() ? entry->GetURL() : GURL();
}

// Whether |contents| was restored (session restore, reopening a closed tab)
// and hasn't navigated since.
bool IsRestoredTab(content::WebContents* contents) {
  content::NavigationEntry* entry =
      contents->GetController().GetLastCommittedEntry();
  return entry && entry->IsRestored();
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
                               SpaceModel* favorites,
                               LiveTabRegistry* registry,
                               RestoredTabMatcher* restored_tab_matcher)
    : browser_(browser),
      tab_strip_model_(browser->GetTabStripModel()),
      space_model_(space_model),
      favorites_(favorites),
      registry_(registry),
      restored_tab_matcher_(restored_tab_matcher) {
  CHECK(tab_strip_model_);
  CHECK(space_model_);
  CHECK(favorites_);
  CHECK(registry_);
  CHECK(restored_tab_matcher_);
  for (int i = 0; i < tab_strip_model_->count(); ++i) {
    AddNodeForTab(tab_strip_model_->GetTabAtIndex(i), i);
  }
  tab_strip_model_->AddObserver(this);
  space_model_->AddObserver(this);
  favorites_->AddObserver(this);
  registry_->AddObserver(this);
  did_become_active_subscription_ =
      browser_->RegisterDidBecomeActive(base::BindRepeating(
          &TabStripBridge::OnWindowBecameActive, base::Unretained(this)));
}

TabStripBridge::~TabStripBridge() {
  registry_->RemoveObserver(this);
  favorites_->RemoveObserver(this);
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
  for (const SidebarNode* root : GetRoots()) {
    if (CountLiveTabsBefore(*this, root, tab_node, index)) {
      break;
    }
  }

  // Live in another window: move it here, the way tab dragging does. That
  // window's bridge keeps the node, and AddNodeForTab() finds it.
  if (tabs::TabInterface* tab = registry_->GetTab(tab_node)) {
    BrowserWindowInterface* source = tab->GetBrowserWindowInterface();
    if (!source) {
      return nullptr;
    }
    // Leave the other window showing the tab, as a placeholder. The parking
    // window shows nothing.
    WindowFeatures* source_features = WindowFeatures::From(source);
    if (source_features && !source_features->is_parking_window()) {
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
  return it == placeholders_.end() ? nullptr : FindNode(it->second);
}

SidebarNode* TabStripBridge::GetShownNode(const tabs::TabInterface* tab) const {
  SidebarNode* node = GetNode(tab);
  return node ? node : GetPlaceholderNode(tab);
}

void TabStripBridge::PrepareToLoseTab(tabs::TabInterface* tab) {
  if (!tab_strip_model_ || tab_strip_model_->GetActiveTab() != tab) {
    return;
  }
  if (SidebarNode* node = GetNode(tab)) {
    CaptureSnapshot(tab, node->id());
    // Just before the tab, so it ends up where the tab was.
    InsertPlaceholder(node, tab_strip_model_->GetIndexOfTab(tab));
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
  base::AutoReset<bool> handing_off(&handing_off_, true);
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
  if (selection.active_tab_changed()) {
    // Auto-archive counts from when an entry was last on screen: the one the
    // window leaves was shown until now. A new tab gets its node, and a time,
    // below.
    const base::Time now = base::Time::Now();
    for (const tabs::TabInterface* tab :
         {selection.old_tab.get(), selection.new_tab.get()}) {
      SidebarNode* node = tab ? GetShownNode(tab) : nullptr;
      if (node && space_model_->Contains(node)) {
        space_model_->SetLastActive(node, now);
      }
    }
  }
  if (selection.active_tab_changed() && selection.new_tab) {
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
        const bool will_delete =
            TabRemoveReasonUtils::WillDeleteTab(removed.remove_reason);
        // The shown tab left without PrepareToLoseTab(): keep showing it.
        // The strip can't change while it notifies, so the placeholder
        // follows in a task; the screenshot request can't wait.
        SidebarNode* node = GetNode(removed.tab);
        if (!will_delete && !handing_off_ && node &&
            removed.tab == selection.old_tab) {
          CaptureSnapshot(removed.tab, node->id());
          base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
              FROM_HERE,
              base::BindOnce(&TabStripBridge::InsertPlaceholderAfterMove,
                             weak_factory_.GetWeakPtr(), node->id(),
                             removed.index));
        }
        RemoveNodeForTab(removed.tab, will_delete);
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

void TabStripBridge::OnNodeAdded(const SidebarNode& node) {
  // A live entry that moved between the space and the favorites.
  if (!updating_ && GetTab(&node)) {
    ScheduleSyncStripOrder();
  }
}

void TabStripBridge::OnNodeMoved(const SidebarNode& node,
                                 const SidebarNode& old_parent,
                                 size_t old_index) {
  if (!updating_) {
    ScheduleSyncStripOrder();
  }
}

void TabStripBridge::OnNodeWillMoveToSpace(const SidebarNode& node) {
  // Its tabs keep the node, and MoveEntryToSpace() moves them out. Placeholders
  // for it have nothing left to bring back here.
  if (!placeholders_.empty()) {
    ScheduleClosePlaceholders();
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

void TabStripBridge::OnNodeLiveChanged(const SidebarNode& node, bool live) {
  // A tab here took over another node (RestoredTabMatcher): its place in the
  // strip follows the new node.
  if (live && !updating_ && GetTab(&node)) {
    ScheduleSyncStripOrder();
  }
}

void TabStripBridge::AddNodeForTab(tabs::TabInterface* tab, int index) {
  if (placeholder_node_) {
    placeholders_[tab->GetHandle()] = placeholder_node_->id();
    placeholder_node_ = nullptr;
    return;
  }
  // Another bridge's placeholder, left from before the window switched
  // spaces. It's about to close.
  if (tab->GetContents()->GetLastCommittedURL() == GURL(kPlaceholderURL)) {
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
  const GURL url = GetTabURL(contents);
  const bool restored = IsRestoredTab(contents);
  base::AutoReset<bool> updating(&updating_, true);
  if (restored) {
    if (SidebarNode* entry = restored_tab_matcher_->FindEntry(url)) {
      registry_->Register(tab, entry);
      return;
    }
  }
  SidebarNode* parent = space_model_->today_root();
  size_t parent_index = 0;
  if (!restored && GetLinkFolderPlace(tab, index, parent, parent_index)) {
    // The strip follows the tree.
    ScheduleSyncStripOrder();
  } else {
    parent_index = GetTodayIndexAfter(index, nullptr);
  }
  SidebarNode* node =
      space_model_->AddTab(parent, parent_index, url, contents->GetTitle());
  registry_->Register(tab, node);
  if (restored) {
    restored_tab_matcher_->AddUnmatched(node);
  }
}

bool TabStripBridge::GetLinkFolderPlace(tabs::TabInterface* tab,
                                        int index,
                                        SidebarNode*& folder,
                                        size_t& folder_index) {
  if (index == tab_strip_model_->active_index()) {
    return false;
  }
  content::NavigationEntry* entry =
      tab->GetContents()->GetController().GetPendingEntry();
  if (!entry) {
    entry = tab->GetContents()->GetController().GetLastCommittedEntry();
  }
  if (!entry || !ui::PageTransitionCoreTypeIs(entry->GetTransitionType(),
                                              ui::PAGE_TRANSITION_LINK)) {
    return false;
  }
  tabs::TabInterface* opener = tab_strip_model_->GetOpenerOfTabAt(index);
  SidebarNode* opener_node = opener ? GetNode(opener) : nullptr;
  if (!opener_node) {
    return false;
  }
  // A split's pane opens next to its split.
  SidebarNode* opener_entry = opener_node;
  if (opener_entry->parent()->type() == SidebarNode::Type::kSplit) {
    opener_entry = opener_entry->parent();
  }
  SidebarNode* today = space_model_->today_root();

  // After the tab before it in the strip, if that's in the folder already
  // (an earlier link of the same opener), else after the opener.
  const auto place_after_previous = [&](SidebarNode* in, size_t fallback) {
    folder = in;
    folder_index = fallback;
    SidebarNode* previous =
        index > 0 ? GetNode(tab_strip_model_->GetTabAtIndex(index - 1))
                  : nullptr;
    while (previous && previous->parent() != in &&
           previous->parent() != today) {
      previous = previous->parent();
    }
    if (previous && previous->parent() == in) {
      folder_index = in->GetIndexOf(previous) + 1;
    }
  };

  base::AutoReset<bool> updating(&updating_, true);
  if (today->Contains(opener_entry)) {
    SidebarNode* parent = opener_entry->parent();
    if (parent->type() != SidebarNode::Type::kFolder) {
      // The opener's own folder takes its place.
      parent = space_model_->AddFolder(today, today->GetIndexOf(opener_entry),
                                       GetLinkFolderTitle(*opener_entry));
      space_model_->Move(opener_entry, parent, 0);
    }
    place_after_previous(parent, parent->GetIndexOf(opener_entry) + 1);
    return true;
  }
  // Pinned entries and favorites stay put; their links gather in a folder
  // at the top of Today.
  auto it = link_folders_.find(opener_node->id());
  SidebarNode* existing =
      it != link_folders_.end() ? space_model_->FindById(it->second) : nullptr;
  if (!existing || !today->Contains(existing) ||
      existing->type() != SidebarNode::Type::kFolder) {
    existing = space_model_->AddFolder(today, 0,
                                       GetLinkFolderTitle(*opener_entry));
    link_folders_[opener_node->id()] = existing->id();
  }
  place_after_previous(existing, existing->children().size());
  return true;
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
  if (!space_model_->Contains(node)) {
    // A favorite, which stays unloaded, or another space's tab, passing
    // through while the window switches.
    return;
  }
  // Tabs close with their window when it's the last one, or when quitting
  // (other windows hand theirs off first). Today keeps them, unloaded, like
  // Pinned does.
  const bool window_closing =
      tab_strip_model_ && tab_strip_model_->closing_all() &&
      browser_->GetType() == BrowserWindowInterface::TYPE_NORMAL;
  if (IsPinned(node) || (window_closing && IsRestorableURL(node->url()))) {
    return;
  }
  base::AutoReset<bool> updating(&updating_, true);
  space_model_->Remove(node);
}

bool TabStripBridge::IsPinned(const SidebarNode* node) const {
  return space_model_->pinned_root()->Contains(node) ||
         favorites_->Contains(node);
}

std::array<const SidebarNode*, 3> TabStripBridge::GetRoots() const {
  return {favorites_->pinned_root(), space_model_->pinned_root(),
          space_model_->today_root()};
}

SidebarNode* TabStripBridge::FindNode(const base::Uuid& id) const {
  SidebarNode* node = space_model_->FindById(id);
  return node ? node : favorites_->FindById(id);
}

void TabStripBridge::UpdateNode(tabs::TabInterface* tab) {
  SidebarNode* node = GetNode(tab);
  if (!node) {
    return;
  }
  SpaceModel* model = favorites_->Contains(node) ? favorites_.get()
                                                  : space_model_.get();
  if (!model->Contains(node)) {
    return;
  }
  content::WebContents* contents = tab->GetContents();
  const GURL url = GetTabURL(contents);
  if (!url.is_empty() && node->url() != url) {
    model->SetUrl(node, url);
  }
  if (node->title() != contents->GetTitle()) {
    model->SetTitle(node, contents->GetTitle());
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
  for (const SidebarNode* root : GetRoots()) {
    CollectLiveTabs(*this, root, order);
  }

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

void TabStripBridge::InsertPlaceholder(SidebarNode* node, int index) {
  if (!node || !tab_strip_model_) {
    return;
  }
  NavigateParams params(browser_, GURL(kPlaceholderURL),
                        ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  params.tabstrip_index = std::min(index, tab_strip_model_->count());
  base::AutoReset<raw_ptr<SidebarNode>> placeholder(&placeholder_node_, node);
  Navigate(&params);
}

void TabStripBridge::InsertPlaceholderAfterMove(const base::Uuid& node_id,
                                                int index) {
  SidebarNode* node = FindNode(node_id);
  // An empty window is closing (its last tab left); a node that's back here
  // needs no placeholder.
  if (!tab_strip_model_ || tab_strip_model_->empty() || !node || GetTab(node)) {
    DropSnapshot(node_id);
    return;
  }
  InsertPlaceholder(node, index);
}

void TabStripBridge::ClosePlaceholders() {
  if (!tab_strip_model_) {
    return;
  }
  std::vector<tabs::TabHandle> to_close;
  for (const auto& [handle, node_id] : placeholders_) {
    tabs::TabInterface* tab = handle.Get();
    if (tab && (tab != tab_strip_model_->GetActiveTab() ||
                !FindNode(node_id))) {
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

void TabStripBridge::ScheduleSyncStripOrder() {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&TabStripBridge::SyncStripOrder,
                                weak_factory_.GetWeakPtr()));
}

}  // namespace strata
