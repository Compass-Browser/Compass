// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/spaces_android.h"

#include <algorithm>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/android/tab_android.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/android/tab_model/tab_model.h"
#include "chrome/browser/ui/android/tab_model/tab_model_list.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/sidebar_store.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkColor.h"
#include "url/gurl.h"

namespace compass {

namespace {

// Chrome for Android's new tab page, which Compass's start page replaces.
bool IsNewTabPage(const GURL& url) {
  return (url.SchemeIs("chrome-native") || url.SchemeIs("chrome")) &&
         url.GetHost() == "newtab";
}

// The desktop's file (window_sync/space_service.cc): the same profile layout.
constexpr base::FilePath::CharType kStoreFileName[] =
    FILE_PATH_LITERAL("Compass Sidebar.json");

const char kUserDataKey[] = "compass_spaces_android";

// The archiver's first run after the spaces load (restored tabs have taken
// their entries by then), and how often it runs after that: the desktop's
// (window_sync/space_service.cc).
constexpr base::TimeDelta kFirstArchiveDelay = base::Seconds(30);
constexpr base::TimeDelta kArchiveInterval = base::Minutes(10);

// How many closed tabs' archived entries are kept for undo.
constexpr size_t kMaxUndoableArchives = 50;

// Whether Chrome's tab |tab_id| is playing sound: the archiver keeps its
// entry, as the desktop's does.
bool IsAudible(int tab_id) {
  for (TabModel* model : TabModelList::models()) {
    if (model->IsOffTheRecord()) {
      continue;
    }
    for (int i = 0; i < model->GetTabCount(); ++i) {
      TabAndroid* tab = model->GetTabAt(i);
      if (tab && tab->GetAndroidId() == tab_id) {
        content::WebContents* contents = tab->GetContents();
        return contents && contents->IsCurrentlyAudible();
      }
    }
  }
  return false;
}

// A Today entry: a tab or split under Today (maybe in a folder), not a
// split's pane.
bool IsTodayEntry(const SpaceModel& space, const SidebarNode& node) {
  return space.today_root() && space.today_root()->Contains(&node) &&
         &node != space.today_root() &&
         (node.type() == SidebarNode::Type::kTab ||
          node.type() == SidebarNode::Type::kSplit) &&
         node.parent()->type() != SidebarNode::Type::kSplit;
}

}  // namespace

// static
SpacesAndroid* SpacesAndroid::Get(Profile* profile) {
  auto* spaces =
      static_cast<SpacesAndroid*>(profile->GetUserData(kUserDataKey));
  if (!spaces) {
    auto owned = std::make_unique<SpacesAndroid>(profile);
    spaces = owned.get();
    profile->SetUserData(kUserDataKey, std::move(owned));
  }
  return spaces;
}

// static
SpacesAndroid* SpacesAndroid::GetIfExists(Profile* profile) {
  return static_cast<SpacesAndroid*>(profile->GetUserData(kUserDataKey));
}

SpacesAndroid::SpacesAndroid(Profile* profile) {
  list_.AddObserver(this);
  list_.favorites().AddObserver(this);
  for (const auto& space : list_.spaces()) {
    space->AddObserver(this);
  }
  store_ = std::make_unique<SidebarStore>(
      profile->GetPath().Append(kStoreFileName), &list_);
  store_->Load(base::BindOnce(&SpacesAndroid::OnLoaded,
                              weak_factory_.GetWeakPtr()));
}

SpacesAndroid::~SpacesAndroid() {
  sync_attachment_.reset();
  store_.reset();
  for (const auto& space : list_.spaces()) {
    space->RemoveObserver(this);
  }
  list_.favorites().RemoveObserver(this);
  list_.RemoveObserver(this);
}

void SpacesAndroid::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void SpacesAndroid::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

SpaceModel& SpacesAndroid::SpaceOrActive(const base::Uuid& space_id) const {
  SpaceModel* space = space_id.is_valid() ? list_.FindSpace(space_id) : nullptr;
  return space ? *space : list_.active_space();
}

std::string SpacesAndroid::Snapshot(const base::Uuid& space_id) const {
  const SpaceModel& shown = SpaceOrActive(space_id);
  base::DictValue snapshot;
  snapshot.Set("loaded", loaded_);
  snapshot.Set("active", shown.id().AsLowercaseString());
  base::ListValue spaces;
  for (const auto& space : list_.spaces()) {
    base::DictValue dict;
    dict.Set("id", space->id().AsLowercaseString());
    dict.Set("name", base::UTF16ToUTF8(space->name()));
    std::string tint;
    if (std::optional<SkColor> color = space->color().tint()) {
      tint = base::StringPrintf("#%02x%02x%02x", SkColorGetR(*color),
                                SkColorGetG(*color), SkColorGetB(*color));
    }
    dict.Set("tint", tint);
    dict.Set("emoji", space->icon().kind() == SpaceIcon::Kind::kEmoji
                          ? space->icon().value()
                          : std::string());
    spaces.Append(std::move(dict));
  }
  snapshot.Set("spaces", std::move(spaces));
  snapshot.Set("favorites", SnapshotSection(*list_.favorites().pinned_root()));
  snapshot.Set("pinned", SnapshotSection(*shown.pinned_root()));
  snapshot.Set("today", SnapshotSection(*shown.today_root()));
  base::ListValue archive;
  for (const ArchivedEntry& entry : shown.archive()) {
    std::u16string title;
    for (const ArchivedEntry::Tab& tab : entry.tabs) {
      if (!title.empty()) {
        title += u" \u00b7 ";
      }
      title += tab.title.empty() ? base::UTF8ToUTF16(tab.url.spec()) : tab.title;
    }
    base::DictValue dict;
    dict.Set("id", entry.id.AsLowercaseString());
    dict.Set("title", base::UTF16ToUTF8(title));
    dict.Set("url", entry.tabs.empty() ? std::string()
                                       : entry.tabs[0].url.spec());
    dict.Set("archived_at",
             entry.archived_at.InMillisecondsFSinceUnixEpochIgnoringNull());
    archive.Append(std::move(dict));
  }
  snapshot.Set("archive", std::move(archive));
  snapshot.Set("archive_after_hours",
               static_cast<int>(shown.archive_after().InHours()));
  return base::WriteJson(snapshot).value_or("{}");
}

base::ListValue SpacesAndroid::SnapshotSection(const SidebarNode& root) const {
  base::ListValue nodes;
  for (const auto& child : root.children()) {
    const SidebarNode& node = *child;
    base::DictValue dict;
    dict.Set("id", node.id().AsLowercaseString());
    switch (node.type()) {
      case SidebarNode::Type::kFolder:
        dict.Set("type", "folder");
        dict.Set("collapsed", node.collapsed());
        // A live folder (docs/design/live-folders.md): its plugin.
        if (node.is_live_folder()) {
          dict.Set("live", node.live_source()->plugin_id);
        }
        break;
      case SidebarNode::Type::kSplit:
        dict.Set("type", "split");
        break;
      default:
        dict.Set("type", "tab");
        if (node.unseen()) {
          dict.Set("unseen", true);
        }
        break;
    }
    dict.Set("title", base::UTF16ToUTF8(node.display_title()));
    dict.Set("url", node.url().spec());
    dict.Set("tab", TabFor(node.id()));
    if (node.saved_url().is_valid()) {
      dict.Set("saved_url", node.saved_url().spec());
    }
    if (!node.children().empty()) {
      dict.Set("children", SnapshotSection(node));
    }
    nodes.Append(std::move(dict));
  }
  return nodes;
}

void SpacesAndroid::TabOpened(int tab_id,
                              const GURL& url,
                              const std::u16string& title,
                              bool restored,
                              const base::Uuid& space_id) {
  if (tabs_.contains(tab_id)) {
    return;
  }
  if (IsNewTabPage(url)) {
    empty_tabs_.insert(tab_id);
    return;
  }
  if (!loaded_) {
    pending_tabs_.push_back({tab_id, url, title, space_id});
    return;
  }
  SpaceModel& space = SpaceOrActive(space_id);
  if (restored) {
    if (SidebarNode* node = FindUnopenedEntry(url)) {
      tabs_[tab_id] = node->id();
      Changed();
      return;
    }
  }
  // New tabs go at the top of Today, as on the desktop.
  SidebarNode* node = space.AddTab(space.today_root(), 0, url, title);
  if (node) {
    tabs_[tab_id] = node->id();
  }
  Changed();
}

void SpacesAndroid::TabOpenedForNode(int tab_id, const base::Uuid& node_id) {
  if (!FindNode(node_id)) {
    return;
  }
  empty_tabs_.erase(tab_id);
  tabs_[tab_id] = node_id;
  Changed();
}

void SpacesAndroid::TabUpdated(int tab_id,
                               const GURL& url,
                               const std::u16string& title) {
  for (PendingTab& pending : pending_tabs_) {
    if (pending.id == tab_id) {
      pending.url = url;
      pending.title = title;
    }
  }
  if (empty_tabs_.contains(tab_id)) {
    // The start page's tab went somewhere: now it's an entry.
    if (IsNewTabPage(url)) {
      return;
    }
    empty_tabs_.erase(tab_id);
    TabOpened(tab_id, url, title, /*restored=*/false);
    return;
  }
  auto it = tabs_.find(tab_id);
  if (it == tabs_.end()) {
    return;
  }
  SidebarNode* node = FindNode(it->second);
  SpaceModel* space = SpaceOf(node);
  if (!node || !space) {
    return;
  }
  if (url.is_valid() && node->url() != url) {
    space->SetUrl(node, url);
  }
  if (!title.empty() && node->title() != title) {
    space->SetTitle(node, title);
  }
}

void SpacesAndroid::RunWhenLoaded(base::OnceClosure callback) {
  if (loaded_) {
    std::move(callback).Run();
  } else {
    on_loaded_.push_back(std::move(callback));
  }
}

void SpacesAndroid::TransferTab(const base::Uuid& from, const base::Uuid& to) {
  if (TabFor(to) != -1) {
    return;
  }
  for (auto& [tab_id, node_id] : tabs_) {
    if (node_id == from) {
      node_id = to;
      Changed();
      return;
    }
  }
}

void SpacesAndroid::TabSelected(int tab_id, const void* window) {
  current_tab_ = tab_id;
  const base::Time now = base::Time::Now();
  auto stamp = [&](int id) {
    auto it = tabs_.find(id);
    if (it == tabs_.end()) {
      return;
    }
    SidebarNode* node = FindNode(it->second);
    if (SpaceModel* space = SpaceOf(node)) {
      space->SetLastActive(node, now);
    }
  };
  // The one the window leaves: its clock starts now.
  auto shown = shown_tabs_.find(window);
  if (shown != shown_tabs_.end() && shown->second != tab_id) {
    stamp(shown->second);
  }
  shown_tabs_[window] = tab_id;
  stamp(tab_id);
}

void SpacesAndroid::WindowClosed(const void* window) {
  auto shown = shown_tabs_.find(window);
  if (shown == shown_tabs_.end()) {
    return;
  }
  auto it = tabs_.find(shown->second);
  shown_tabs_.erase(shown);
  if (it == tabs_.end()) {
    return;
  }
  SidebarNode* node = FindNode(it->second);
  if (SpaceModel* space = SpaceOf(node)) {
    space->SetLastActive(node, base::Time::Now());
  }
}

void SpacesAndroid::TabClosed(int tab_id) {
  empty_tabs_.erase(tab_id);
  std::erase_if(pending_tabs_,
                [&](const PendingTab& pending) { return pending.id == tab_id; });
  auto it = tabs_.find(tab_id);
  if (it == tabs_.end()) {
    return;
  }
  base::Uuid node_id = it->second;
  tabs_.erase(it);
  SidebarNode* node = FindNode(node_id);
  SpaceModel* space = SpaceOf(node);
  if (!node || !space) {
    Changed();
    return;
  }
  // A Today entry goes to the archive with its tab, as on iOS; a split's
  // pane just goes.
  if (IsTodayEntry(*space, *node)) {
    space->Archive(node, base::Time::Now());
    if (space->FindArchived(node_id)) {
      archived_closed_tabs_[tab_id] = node_id;
      if (archived_closed_tabs_.size() > kMaxUndoableArchives) {
        // Tab IDs only grow: the smallest is the oldest.
        archived_closed_tabs_.erase(archived_closed_tabs_.begin());
      }
    }
    Changed();
    return;
  }
  if (space->today_root() && space->today_root()->Contains(node)) {
    space->Remove(node);
    Changed();
    return;
  }
  // A pinned tab or favorite goes back to its pinned URL, unloaded.
  if (node->saved_url().is_valid() && node->url() != node->saved_url()) {
    space->SetUrl(node, node->saved_url());
  }
  Changed();
}

void SpacesAndroid::TabClosureUndone(int tab_id,
                                     const GURL& url,
                                     const std::u16string& title,
                                     const base::Uuid& space_id) {
  auto it = archived_closed_tabs_.find(tab_id);
  if (it != archived_closed_tabs_.end()) {
    const base::Uuid entry_id = it->second;
    archived_closed_tabs_.erase(it);
    for (const auto& space : list_.spaces()) {
      if (!space->FindArchived(entry_id)) {
        continue;
      }
      SidebarNode* node = space->Unarchive(entry_id, 0);
      if (node && node->type() == SidebarNode::Type::kTab) {
        empty_tabs_.erase(tab_id);
        tabs_[tab_id] = node->id();
        Changed();
        return;
      }
      break;
    }
  }
  // Its entry is gone (it wasn't worth archiving, or sync dropped it): a new
  // one, at the top of Today.
  TabOpened(tab_id, url, title, /*restored=*/false, space_id);
}

void SpacesAndroid::ArchiveNode(const base::Uuid& node_id) {
  SidebarNode* node = FindNode(node_id);
  SpaceModel* space = SpaceOf(node);
  if (!node || !space || !IsTodayEntry(*space, *node)) {
    return;
  }
  std::map<int, base::Uuid> before = tabs_;
  space->Archive(node, base::Time::Now());
  std::vector<int> let_go;
  for (const auto& [tab_id, id] : before) {
    if (!tabs_.contains(tab_id)) {
      let_go.push_back(tab_id);
    }
  }
  if (!let_go.empty()) {
    for (Observer& observer : observers_) {
      observer.OnTabsArchived(let_go);
    }
  }
  Changed();
}

SidebarNode* SpacesAndroid::RestoreArchived(const base::Uuid& entry_id) {
  for (const auto& space : list_.spaces()) {
    if (!space->FindArchived(entry_id)) {
      continue;
    }
    SidebarNode* node = space->Unarchive(entry_id, 0);
    Changed();
    if (node && node->type() == SidebarNode::Type::kSplit) {
      return node->children().empty() ? nullptr
                                       : node->children().front().get();
    }
    return node;
  }
  return nullptr;
}

void SpacesAndroid::RemoveArchived(const base::Uuid& entry_id) {
  for (const auto& space : list_.spaces()) {
    if (space->FindArchived(entry_id)) {
      space->RemoveArchived(entry_id);
      Changed();
      return;
    }
  }
}

void SpacesAndroid::SetArchiveAfter(const base::Uuid& space_id, int hours) {
  if (SpaceModel* space = list_.FindSpace(space_id)) {
    space->SetArchiveAfter(base::Hours(std::max(hours, 0)));
  }
}

bool SpacesAndroid::IsPlayingSound(const SidebarNode& entry) const {
  if (int tab_id = TabFor(entry.id()); tab_id != -1 && IsAudible(tab_id)) {
    return true;
  }
  for (const auto& child : entry.children()) {
    if (int tab_id = TabFor(child->id()); tab_id != -1 && IsAudible(tab_id)) {
      return true;
    }
  }
  return false;
}

bool SpacesAndroid::IsShown(const SidebarNode& entry) {
  for (const auto& [window, tab_id] : shown_tabs_) {
    auto it = tabs_.find(tab_id);
    if (it == tabs_.end()) {
      continue;
    }
    if (const SidebarNode* node = FindNode(it->second);
        node && entry.Contains(node)) {
      return true;
    }
  }
  return false;
}

void SpacesAndroid::ArchiveIdleEntries() {
  if (!loaded_) {
    return;
  }
  std::map<int, base::Uuid> before = tabs_;
  const base::Time now = base::Time::Now();
  size_t archived = 0;
  for (const auto& space : list_.spaces()) {
    archived += space->ArchiveIdleEntries(
        now, [&](const SidebarNode& entry) {
          return IsShown(entry) || IsPlayingSound(entry);
        });
  }
  if (!archived) {
    return;
  }
  // The archived entries' open tabs close (OnNodeWillBeRemoved() let them
  // go).
  std::vector<int> closed;
  for (const auto& [tab_id, id] : before) {
    if (!tabs_.contains(tab_id)) {
      closed.push_back(tab_id);
    }
  }
  if (!closed.empty()) {
    for (Observer& observer : observers_) {
      observer.OnTabsArchived(closed);
    }
  }
  Changed();
}

void SpacesAndroid::UseCurrentPageAsSavedUrl(const base::Uuid& node_id) {
  SidebarNode* node = FindNode(node_id);
  SpaceModel* space = SpaceOf(node);
  if (!node || !space || node->type() != SidebarNode::Type::kTab ||
      !node->saved_url().is_valid() || !node->url().is_valid()) {
    return;
  }
  space->SetSavedUrl(node, node->url(), node->title());
}

void SpacesAndroid::Pin(int tab_id, bool favorite) {
  auto it = tabs_.find(tab_id);
  if (it == tabs_.end()) {
    return;
  }
  SidebarNode* node = FindNode(it->second);
  SpaceModel* space = SpaceOf(node);
  if (!node || !space) {
    return;
  }
  MoveNode(node->id(), space->id(),
           favorite ? Section::kFavorites : Section::kPinned);
}

void SpacesAndroid::MoveNode(const base::Uuid& node_id,
                             const base::Uuid& space_id,
                             Section section) {
  SidebarNode* node = FindNode(node_id);
  SpaceModel* from = SpaceOf(node);
  if (!node || !from) {
    return;
  }
  SpaceModel* target = section == Section::kFavorites
                           ? &list_.favorites()
                           : list_.FindSpace(space_id);
  if (!target) {
    return;
  }
  // The favorites have no Today.
  SidebarNode* parent = section == Section::kToday && target->today_root()
                            ? target->today_root()
                            : target->pinned_root();
  size_t index = parent == target->today_root() ? 0 : parent->children().size();
  if (target == from) {
    if (from->CanMove(node, parent)) {
      from->Move(node, parent, index);
    }
  } else if (from->CanMoveToSpace(node, *target, parent)) {
    from->MoveToSpace(node, *target, parent, index);
  }
}

int SpacesAndroid::TabFor(const base::Uuid& node_id) const {
  for (const auto& [tab_id, id] : tabs_) {
    if (id == node_id) {
      return tab_id;
    }
  }
  return -1;
}

SidebarNode* SpacesAndroid::FindNode(const base::Uuid& id) {
  if (SidebarNode* node = list_.favorites().FindById(id)) {
    return node;
  }
  for (const auto& space : list_.spaces()) {
    if (SidebarNode* node = space->FindById(id)) {
      return node;
    }
  }
  return nullptr;
}

SpaceModel* SpacesAndroid::SpaceOf(const SidebarNode* node) {
  if (!node) {
    return nullptr;
  }
  if (list_.favorites().Contains(node)) {
    return &list_.favorites();
  }
  return list_.FindSpaceOf(node);
}

SidebarNode* SpacesAndroid::FindUnopenedEntry(const GURL& url) {
  SidebarNode* found = nullptr;
  auto search = [&](auto& self, const SidebarNode& parent) -> void {
    for (const auto& child : parent.children()) {
      if (found) {
        return;
      }
      if (child->type() == SidebarNode::Type::kTab && child->url() == url &&
          TabFor(child->id()) == -1) {
        found = child.get();
        return;
      }
      self(self, *child);
    }
  };
  search(search, *list_.favorites().pinned_root());
  for (const auto& space : list_.spaces()) {
    search(search, *space->pinned_root());
  }
  for (const auto& space : list_.spaces()) {
    search(search, *space->today_root());
  }
  return found;
}

void SpacesAndroid::OnLoaded() {
  loaded_ = true;
  std::vector<PendingTab> pending;
  pending.swap(pending_tabs_);
  for (const PendingTab& tab : pending) {
    TabOpened(tab.id, tab.url, tab.title, /*restored=*/true, tab.space_id);
  }
  first_archive_timer_.Start(FROM_HERE, kFirstArchiveDelay, this,
                             &SpacesAndroid::ArchiveIdleEntries);
  archive_timer_.Start(FROM_HERE, kArchiveInterval, this,
                       &SpacesAndroid::ArchiveIdleEntries);
  std::vector<base::OnceClosure> callbacks;
  callbacks.swap(on_loaded_);
  for (base::OnceClosure& callback : callbacks) {
    std::move(callback).Run();
  }
  Changed();
}

void SpacesAndroid::Changed() {
  if (notify_pending_) {
    return;
  }
  notify_pending_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&SpacesAndroid::NotifyChanged,
                                weak_factory_.GetWeakPtr()));
}

void SpacesAndroid::NotifyChanged() {
  notify_pending_ = false;
  for (Observer& observer : observers_) {
    observer.OnSpacesChanged();
  }
}

void SpacesAndroid::OnSpaceAdded(SpaceModel& space) {
  space.AddObserver(this);
  Changed();
}

void SpacesAndroid::OnSpaceWillBeRemoved(SpaceModel& space) {
  space.RemoveObserver(this);
  Changed();
}

void SpacesAndroid::OnSpaceUpdated(const SpaceModel& space) {
  Changed();
}

void SpacesAndroid::OnSpaceMoved(const SpaceModel& space) {
  Changed();
}

void SpacesAndroid::OnActiveSpaceChanged() {
  Changed();
}

void SpacesAndroid::OnNodeAdded(const SidebarNode& node) {
  Changed();
}

void SpacesAndroid::OnNodeMoved(const SidebarNode& node,
                                const SidebarNode& old_parent,
                                size_t old_index) {
  Changed();
}

void SpacesAndroid::OnNodeChanged(const SidebarNode& node) {
  Changed();
}

void SpacesAndroid::OnNodeWillBeRemoved(const SidebarNode& node) {
  // Its tabs (and those of its subtree) no longer show a node.
  std::erase_if(tabs_, [&](const auto& entry) {
    for (const SidebarNode* n = FindNode(entry.second); n; n = n->parent()) {
      if (n == &node) {
        return true;
      }
    }
    return false;
  });
  Changed();
}

void SpacesAndroid::OnSpaceChanged(const SpaceModel& space) {
  Changed();
}

void SpacesAndroid::OnArchiveChanged(const SpaceModel& space) {
  Changed();
}

}  // namespace compass
