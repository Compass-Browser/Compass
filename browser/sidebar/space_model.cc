// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/sidebar/space_model.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/notreached.h"
#include "base/uuid.h"
#include "strata/browser/sidebar/sidebar_serializer.h"
#include "url/gurl.h"

namespace strata {

namespace {

using Type = SidebarNode::Type;

bool IsPane(const SidebarNode* node) {
  return node->type() == Type::kTab && node->parent() &&
         node->parent()->type() == Type::kSplit;
}

SidebarNode* FindIn(SidebarNode* node, const base::Uuid& id) {
  if (node->id() == id) {
    return node;
  }
  for (const auto& child : node->children()) {
    if (SidebarNode* found = FindIn(child.get(), id)) {
      return found;
    }
  }
  return nullptr;
}

// Appends the tabs under |node| to |tabs|: the node itself, or a split's
// panes.
void CollectTabs(SidebarNode* node, std::vector<SidebarNode*>& tabs) {
  if (node->type() == Type::kTab) {
    tabs.push_back(node);
  }
  for (const auto& child : node->children()) {
    CollectTabs(child.get(), tabs);
  }
}

}  // namespace

ArchivedEntry::ArchivedEntry() = default;
ArchivedEntry::ArchivedEntry(const ArchivedEntry&) = default;
ArchivedEntry::ArchivedEntry(ArchivedEntry&&) = default;
ArchivedEntry& ArchivedEntry::operator=(const ArchivedEntry&) = default;
ArchivedEntry& ArchivedEntry::operator=(ArchivedEntry&&) = default;
ArchivedEntry::~ArchivedEntry() = default;

SpaceModel::SpaceModel(Kind kind)
    : kind_(kind),
      id_(base::Uuid::GenerateRandomV4()),
      pinned_root_(
          std::make_unique<SidebarNode>(Type::kRoot, std::u16string(), GURL())),
      today_root_(std::make_unique<SidebarNode>(Type::kRoot,
                                                std::u16string(),
                                                GURL())) {}

SpaceModel::~SpaceModel() = default;

void SpaceModel::SetName(std::u16string name) {
  if (name_ == name) {
    return;
  }
  name_ = std::move(name);
  for (Observer& observer : observers_) {
    observer.OnSpaceChanged(*this);
  }
}

void SpaceModel::SetColor(SpaceColor color) {
  if (color_ == color) {
    return;
  }
  color_ = color;
  for (Observer& observer : observers_) {
    observer.OnSpaceChanged(*this);
  }
}

void SpaceModel::SetArchiveAfter(base::TimeDelta archive_after) {
  CHECK(!archive_after.is_negative());
  if (archive_after_ == archive_after) {
    return;
  }
  archive_after_ = archive_after;
  for (Observer& observer : observers_) {
    observer.OnSpaceChanged(*this);
  }
}

void SpaceModel::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void SpaceModel::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

SidebarNode* SpaceModel::AddFolder(SidebarNode* parent,
                                   size_t index,
                                   std::u16string title) {
  return AddNode(parent, index,
                 std::make_unique<SidebarNode>(Type::kFolder, std::move(title),
                                               GURL()));
}

SidebarNode* SpaceModel::AddTab(SidebarNode* parent,
                                size_t index,
                                const GURL& url,
                                std::u16string title) {
  return AddNode(
      parent, index,
      std::make_unique<SidebarNode>(Type::kTab, std::move(title), url));
}

bool SpaceModel::CanMove(const SidebarNode* node,
                         const SidebarNode* new_parent) const {
  if (!node || !new_parent || node->type() == Type::kRoot ||
      node->Contains(new_parent)) {
    return false;
  }
  return node->parent() == new_parent ||
         CanContain(new_parent, node->type());
}

void SpaceModel::Move(SidebarNode* node, SidebarNode* new_parent,
                      size_t index) {
  CHECK(CanMove(node, new_parent));
  SidebarNode* old_parent = node->parent();
  const size_t old_index = old_parent->GetIndexOf(node);
  new_parent->Add(old_parent->Remove(node), index);
  for (auto& observer : observers_) {
    observer.OnNodeMoved(*node, *old_parent, old_index);
  }
  if (old_parent != new_parent && old_parent->type() == Type::kSplit) {
    NormalizeSplit(old_parent);
  }
}

bool SpaceModel::CanMoveToSpace(const SidebarNode* node,
                                const SpaceModel& target,
                                const SidebarNode* new_parent) const {
  return node && new_parent && &target != this && Contains(node) &&
         node->type() != Type::kRoot && target.Contains(new_parent) &&
         target.CanContain(new_parent, node->type());
}

void SpaceModel::MoveToSpace(SidebarNode* node,
                             SpaceModel& target,
                             SidebarNode* new_parent,
                             size_t index) {
  CHECK(CanMoveToSpace(node, target, new_parent));
  for (auto& observer : observers_) {
    observer.OnNodeWillMoveToSpace(*node);
  }
  SidebarNode* parent = node->parent();
  target.AddNode(new_parent, index, parent->Remove(node));
  if (parent->type() == Type::kSplit) {
    NormalizeSplit(parent);
  }
}

bool SpaceModel::CanSplit(const SidebarNode* target,
                          const SidebarNode* tab) const {
  if (!target || !tab || tab->type() != Type::kTab ||
      kind_ == Kind::kFavorites) {
    return false;
  }
  if (IsPane(target)) {
    target = target->parent();
  }
  if (target->Contains(tab)) {
    return false;
  }
  switch (target->type()) {
    case Type::kTab:
      return true;
    case Type::kSplit:
      return target->children().size() < kMaxSplitPanes;
    case Type::kRoot:
    case Type::kFolder:
      return false;
  }
  NOTREACHED();
}

SidebarNode* SpaceModel::Split(SidebarNode* target, SidebarNode* tab) {
  CHECK(CanSplit(target, tab));
  if (IsPane(target)) {
    target = target->parent();
  }
  if (target->type() == Type::kSplit) {
    Move(tab, target, target->children().size());
    return target;
  }
  SidebarNode* parent = target->parent();
  SidebarNode* split = AddNode(
      parent, parent->GetIndexOf(target),
      std::make_unique<SidebarNode>(Type::kSplit, std::u16string(), GURL()));
  Move(target, split, 0);
  Move(tab, split, 1);
  return split;
}

void SpaceModel::Remove(SidebarNode* node) {
  CHECK(node);
  CHECK_NE(node->type(), Type::kRoot);
  for (auto& observer : observers_) {
    observer.OnNodeWillBeRemoved(*node);
  }
  SidebarNode* parent = node->parent();
  parent->Remove(node);
  if (parent->type() == Type::kSplit) {
    NormalizeSplit(parent);
  }
}

void SpaceModel::SetTitle(SidebarNode* node, std::u16string title) {
  CHECK_NE(node->type(), Type::kRoot);
  node->title_ = std::move(title);
  for (auto& observer : observers_) {
    observer.OnNodeChanged(*node);
  }
}

void SpaceModel::SetCustomTitle(SidebarNode* tab, std::u16string title) {
  CHECK_EQ(tab->type(), Type::kTab);
  if (tab->custom_title_ == title) {
    return;
  }
  tab->custom_title_ = std::move(title);
  for (auto& observer : observers_) {
    observer.OnNodeChanged(*tab);
  }
}

void SpaceModel::SetUrl(SidebarNode* tab, const GURL& url) {
  CHECK_EQ(tab->type(), Type::kTab);
  tab->url_ = url;
  for (auto& observer : observers_) {
    observer.OnNodeChanged(*tab);
  }
}

void SpaceModel::SetCollapsed(SidebarNode* folder, bool collapsed) {
  CHECK_EQ(folder->type(), Type::kFolder);
  if (folder->collapsed_ == collapsed) {
    return;
  }
  folder->collapsed_ = collapsed;
  for (auto& observer : observers_) {
    observer.OnNodeChanged(*folder);
  }
}

void SpaceModel::SetLastActive(SidebarNode* tab, base::Time time) {
  CHECK_EQ(tab->type(), Type::kTab);
  if (tab->last_active_ == time) {
    return;
  }
  tab->last_active_ = time;
  for (auto& observer : observers_) {
    observer.OnNodeLastActiveChanged(*tab);
  }
}

void SpaceModel::Archive(SidebarNode* entry, base::Time now) {
  CHECK(entry);
  CHECK_EQ(entry->parent(), today_root_.get());
  std::vector<SidebarNode*> tabs;
  CollectTabs(entry, tabs);
  ArchivedEntry archived;
  archived.archived_at = now;
  for (const SidebarNode* tab : tabs) {
    if (IsRestorableURL(tab->url())) {
      archived.tabs.push_back({tab->title(), tab->url()});
    }
  }
  Remove(entry);
  if (archived.tabs.empty()) {
    return;
  }
  archive_.insert(archive_.begin(), std::move(archived));
  if (archive_.size() > kMaxArchivedEntries) {
    archive_.resize(kMaxArchivedEntries);
  }
  NotifyArchiveChanged();
}

size_t SpaceModel::ArchiveIdleEntries(
    base::Time now,
    base::FunctionRef<bool(const SidebarNode&)> keep) {
  if (archive_after_.is_zero()) {
    return 0;
  }
  std::vector<base::Uuid> idle;
  for (const auto& entry : today_root_->children()) {
    std::vector<SidebarNode*> tabs;
    CollectTabs(entry.get(), tabs);
    base::Time last_active;
    for (SidebarNode* tab : tabs) {
      if (tab->last_active().is_null()) {
        SetLastActive(tab, now);
      }
      last_active = std::max(last_active, tab->last_active());
    }
    if (!tabs.empty() && now - last_active >= archive_after_ &&
        !keep(*entry)) {
      idle.push_back(entry->id());
    }
  }
  // Archiving closes live tabs, which can change Today, so go by ID.
  size_t archived = 0;
  for (const base::Uuid& id : idle) {
    SidebarNode* entry = FindIn(today_root_.get(), id);
    if (entry && entry->parent() == today_root_.get()) {
      Archive(entry, now);
      ++archived;
    }
  }
  return archived;
}

SidebarNode* SpaceModel::Unarchive(const base::Uuid& id, size_t index) {
  auto it = std::ranges::find(archive_, id, &ArchivedEntry::id);
  if (it == archive_.end()) {
    return nullptr;
  }
  const ArchivedEntry archived = std::move(*it);
  archive_.erase(it);
  index = std::min(index, today_root_->children().size());
  std::vector<SidebarNode*> panes;
  for (const ArchivedEntry::Tab& tab : archived.tabs) {
    if (panes.size() == kMaxSplitPanes) {
      break;
    }
    panes.push_back(
        AddTab(today_root_.get(), index + panes.size(), tab.url, tab.title));
  }
  for (size_t i = 1; i < panes.size(); ++i) {
    Split(panes[0], panes[i]);
  }
  NotifyArchiveChanged();
  if (panes.empty()) {
    return nullptr;
  }
  return panes.size() == 1 ? panes[0] : panes[0]->parent();
}

void SpaceModel::RemoveArchived(const base::Uuid& id) {
  if (std::erase_if(archive_, [&id](const ArchivedEntry& entry) {
        return entry.id == id;
      })) {
    NotifyArchiveChanged();
  }
}

void SpaceModel::ClearArchive() {
  if (archive_.empty()) {
    return;
  }
  archive_.clear();
  NotifyArchiveChanged();
}

void SpaceModel::AddArchived(ArchivedEntry entry) {
  if (archive_.size() >= kMaxArchivedEntries || entry.tabs.empty()) {
    return;
  }
  archive_.push_back(std::move(entry));
  NotifyArchiveChanged();
}

void SpaceModel::NotifyArchiveChanged() {
  for (auto& observer : observers_) {
    observer.OnArchiveChanged(*this);
  }
}

SidebarNode* SpaceModel::FindById(const base::Uuid& id) {
  if (SidebarNode* found = FindIn(pinned_root_.get(), id)) {
    return found;
  }
  return FindIn(today_root_.get(), id);
}

bool SpaceModel::Contains(const SidebarNode* node) const {
  return pinned_root_->Contains(node) || today_root_->Contains(node);
}

bool SpaceModel::CanContain(const SidebarNode* parent, Type type) const {
  if (kind_ == Kind::kFavorites) {
    return parent == pinned_root_.get() && type == Type::kTab;
  }
  switch (parent->type()) {
    case Type::kRoot:
      return type == Type::kTab || type == Type::kSplit ||
             (type == Type::kFolder && parent == pinned_root_.get());
    case Type::kFolder:
      return type != Type::kRoot;
    case Type::kSplit:
      return type == Type::kTab &&
             parent->children().size() < kMaxSplitPanes;
    case Type::kTab:
      return false;
  }
  NOTREACHED();
}

void SpaceModel::NormalizeSplit(SidebarNode* split) {
  CHECK_EQ(split->type(), Type::kSplit);
  if (split->children().empty()) {
    Remove(split);
  } else if (split->children().size() == 1) {
    // Moving the last pane out empties the split, and Move() removes it.
    SidebarNode* parent = split->parent();
    Move(split->children()[0].get(), parent, parent->GetIndexOf(split));
  }
}

SidebarNode* SpaceModel::AddNode(SidebarNode* parent,
                                 size_t index,
                                 std::unique_ptr<SidebarNode> node) {
  CHECK(parent);
  CHECK(CanContain(parent, node->type()));
  SidebarNode* added = parent->Add(std::move(node), index);
  for (auto& observer : observers_) {
    observer.OnNodeAdded(*added);
  }
  return added;
}

}  // namespace strata
