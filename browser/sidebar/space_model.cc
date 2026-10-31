// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/space_model.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversion_utils.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "compass/browser/sidebar/sidebar_serializer.h"
#include "url/gurl.h"

namespace compass {

SpaceIcon::SpaceIcon() = default;

SpaceIcon::SpaceIcon(Kind kind, std::string value)
    : kind_(kind), value_(std::move(value)) {}

SpaceIcon::SpaceIcon(const SpaceIcon&) = default;
SpaceIcon& SpaceIcon::operator=(const SpaceIcon&) = default;
SpaceIcon::~SpaceIcon() = default;

// static
std::optional<SpaceIcon> SpaceIcon::Emoji(std::string_view emoji) {
  if (emoji.empty() || emoji.size() > kMaxEmojiBytes ||
      !base::IsStringUTF8(emoji)) {
    return std::nullopt;
  }
  // No whitespace, ASCII controls or C1 controls (U+0080-U+009F).
  std::u16string utf16 = base::UTF8ToUTF16(emoji);
  for (char16_t c : utf16) {
    if (c < 0x20 || (c >= 0x7f && c < 0xa0) || base::IsUnicodeWhitespace(c)) {
      return std::nullopt;
    }
  }
  return SpaceIcon(Kind::kEmoji, std::string(emoji));
}

// static
std::optional<SpaceIcon> SpaceIcon::Symbol(std::string_view name) {
  if (name.empty() || name.size() > kMaxSymbolBytes ||
      !std::ranges::all_of(name, [](char c) {
        return base::IsAsciiLower(c) || base::IsAsciiDigit(c) || c == '-';
      })) {
    return std::nullopt;
  }
  return SpaceIcon(Kind::kSymbol, std::string(name));
}

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

// The entries under |node|: tabs and splits, in folders too.
void CollectEntries(SidebarNode* node, std::vector<SidebarNode*>& entries) {
  for (const auto& child : node->children()) {
    if (child->type() == Type::kFolder) {
      CollectEntries(child.get(), entries);
    } else {
      entries.push_back(child.get());
    }
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
  // Grey isn't tinted, so it has no gradient.
  if (color_.is_grey()) {
    gradient_.clear();
  }
  for (Observer& observer : observers_) {
    observer.OnSpaceChanged(*this);
  }
}

void SpaceModel::SetGradient(std::vector<SpaceColor> gradient) {
  std::erase_if(gradient, [](SpaceColor color) { return color.is_grey(); });
  if (color_.is_grey()) {
    gradient.clear();
  }
  if (gradient.size() > kMaxSpaceGradientColors - 1) {
    gradient.resize(kMaxSpaceGradientColors - 1);
  }
  if (gradient_ == gradient) {
    return;
  }
  gradient_ = std::move(gradient);
  for (Observer& observer : observers_) {
    observer.OnSpaceChanged(*this);
  }
}

void SpaceModel::SetIntensity(double intensity) {
  if (std::isnan(intensity)) {
    return;
  }
  intensity = std::clamp(intensity, 0.0, 1.0);
  if (intensity_ == intensity) {
    return;
  }
  intensity_ = intensity;
  for (Observer& observer : observers_) {
    observer.OnSpaceChanged(*this);
  }
}

void SpaceModel::SetGrain(double grain) {
  if (std::isnan(grain)) {
    return;
  }
  grain = std::clamp(grain, 0.0, 1.0);
  if (grain_ == grain) {
    return;
  }
  grain_ = grain;
  for (Observer& observer : observers_) {
    observer.OnSpaceChanged(*this);
  }
}

void SpaceModel::SetIcon(SpaceIcon icon) {
  if (icon_ == icon) {
    return;
  }
  icon_ = std::move(icon);
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

void SpaceModel::Unsplit(SidebarNode* split) {
  CHECK_EQ(split->type(), Type::kSplit);
  // The first pane leaving makes NormalizeSplit() move the last one out
  // after it, and remove the split.
  SidebarNode* parent = split->parent();
  Move(split->children()[0].get(), parent, parent->GetIndexOf(split));
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
  CHECK(today_root_->Contains(entry));
  // Folders aren't archived; their entries are, one by one.
  CHECK(entry->type() == Type::kTab || entry->type() == Type::kSplit);
  CHECK_NE(entry->parent()->type(), Type::kSplit);
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
  std::vector<SidebarNode*> entries;
  CollectEntries(today_root_.get(), entries);
  for (SidebarNode* entry : entries) {
    std::vector<SidebarNode*> tabs;
    CollectTabs(entry, tabs);
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
    if (entry && entry->type() != Type::kFolder &&
        entry->parent()->type() != Type::kSplit) {
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
             type == Type::kFolder;
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

}  // namespace compass
