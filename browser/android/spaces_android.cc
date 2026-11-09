// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/spaces_android.h"

#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/sidebar_store.h"
#include "third_party/skia/include/core/SkColor.h"
#include "url/gurl.h"

namespace compass {

namespace {

// The desktop's file (window_sync/space_service.cc): the same profile layout.
constexpr base::FilePath::CharType kStoreFileName[] =
    FILE_PATH_LITERAL("Compass Sidebar.json");

const char kUserDataKey[] = "compass_spaces_android";

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

std::string SpacesAndroid::Snapshot() const {
  base::DictValue snapshot;
  snapshot.Set("loaded", loaded_);
  snapshot.Set("active", list_.active_space().id().AsLowercaseString());
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
  snapshot.Set("pinned", SnapshotSection(*list_.active_space().pinned_root()));
  snapshot.Set("today", SnapshotSection(*list_.active_space().today_root()));
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
        break;
      case SidebarNode::Type::kSplit:
        dict.Set("type", "split");
        break;
      default:
        dict.Set("type", "tab");
        break;
    }
    dict.Set("title", base::UTF16ToUTF8(node.display_title()));
    dict.Set("url", node.url().spec());
    dict.Set("tab", TabFor(node.id()));
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
                              bool restored) {
  if (tabs_.contains(tab_id)) {
    return;
  }
  if (!loaded_) {
    pending_tabs_.push_back({tab_id, url, title});
    return;
  }
  SpaceModel& space = list_.active_space();
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

void SpacesAndroid::TabSelected(int tab_id) {
  current_tab_ = tab_id;
  auto it = tabs_.find(tab_id);
  if (it == tabs_.end()) {
    return;
  }
  SidebarNode* node = FindNode(it->second);
  if (SpaceModel* space = SpaceOf(node)) {
    space->SetLastActive(node, base::Time::Now());
  }
}

void SpacesAndroid::TabClosed(int tab_id) {
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
  // A Today entry goes with its tab.
  for (const SidebarNode* n = node; n; n = n->parent()) {
    if (n == space->today_root()) {
      space->Remove(node);
      Changed();
      return;
    }
  }
  // A pinned tab or favorite goes back to its pinned URL, unloaded.
  if (node->saved_url().is_valid() && node->url() != node->saved_url()) {
    space->SetUrl(node, node->saved_url());
  }
  Changed();
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
  SpaceModel& target = favorite ? list_.favorites() : *space;
  SidebarNode* parent = target.pinned_root();
  if (&target == space) {
    if (space->CanMove(node, parent)) {
      space->Move(node, parent, parent->children().size());
    }
  } else if (space->CanMoveToSpace(node, target, parent)) {
    space->MoveToSpace(node, target, parent, parent->children().size());
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
    TabOpened(tab.id, tab.url, tab.title, /*restored=*/true);
  }
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
