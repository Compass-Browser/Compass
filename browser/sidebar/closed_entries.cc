// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/closed_entries.h"

#include <algorithm>
#include <utility>

#include "base/check_op.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/sidebar_serializer.h"
#include "compass/browser/sidebar/space_model.h"

namespace compass {

namespace {

using Type = SidebarNode::Type;

void CollectIds(const SidebarNode& node, std::vector<base::Uuid>& ids) {
  ids.push_back(node.id());
  for (const auto& child : node.children()) {
    CollectIds(*child, ids);
  }
}

// Puts the removed entry |entry| back into |model|.
void RestoreRemoved(const ClosedEntries::Entry& entry, SpaceModel& model) {
  // A pane whose split ended as it left: back next to the other pane, and
  // split with it again, in the order they had.
  if (entry.split_sibling_id.is_valid() && !model.FindById(entry.parent_id)) {
    SidebarNode* sibling = model.FindById(entry.split_sibling_id);
    if (sibling && sibling->type() == Type::kTab &&
        sibling->parent()->type() != Type::kSplit) {
      SidebarNode* parent = sibling->parent();
      const size_t index =
          parent->GetIndexOf(sibling) + (entry.index == 0 ? 0 : 1);
      if (SidebarNode* pane =
              DeserializeEntry(*entry.node, model, parent, index)) {
        if (model.CanSplit(sibling, pane)) {
          if (entry.index == 0) {
            model.Split(pane, sibling);
          } else {
            model.Split(sibling, pane);
          }
        }
        return;
      }
    }
  }

  SidebarNode* parent = model.FindById(entry.parent_id);
  size_t index = entry.index;
  if (parent) {
    // After the sibling it followed, if that's still there: earlier entries
    // of the step come back first.
    if (!entry.previous_id.is_valid()) {
      index = 0;
    } else if (SidebarNode* previous = model.FindById(entry.previous_id);
               previous && previous->parent() == parent) {
      index = parent->GetIndexOf(previous) + 1;
    }
    if (DeserializeEntry(*entry.node, model, parent, index)) {
      return;
    }
  }
  // Its parent is gone: the top of Today, where new entries go, or the end
  // of Pinned.
  SidebarNode* root = entry.in_today && model.kind() == SpaceModel::Kind::kSpace
                          ? model.today_root()
                          : model.pinned_root();
  DeserializeEntry(*entry.node, model, root,
                   root == model.today_root() ? 0 : root->children().size());
}

}  // namespace

ClosedEntries::Entry::Entry() = default;
ClosedEntries::Entry::Entry(Entry&&) = default;
ClosedEntries::Entry& ClosedEntries::Entry::operator=(Entry&&) = default;
ClosedEntries::Entry::~Entry() = default;

ClosedEntries::Step::Step() = default;
ClosedEntries::Step::Step(Step&&) = default;
ClosedEntries::Step& ClosedEntries::Step::operator=(Step&&) = default;
ClosedEntries::Step::~Step() = default;

bool ClosedEntries::Step::Contains(const base::Uuid& id) const {
  return std::ranges::any_of(entries, [&id](const Entry& entry) {
    return std::ranges::contains(entry.ids, id);
  });
}

ClosedEntries::ScopedStep::ScopedStep(ClosedEntries& closed, bool keep_redo)
    : closed_(closed) {
  if (closed_->open_steps_++ == 0) {
    closed_->step_started_ = false;
    closed_->keep_redo_ = keep_redo;
  }
}

ClosedEntries::ScopedStep::~ScopedStep() {
  CHECK_GT(closed_->open_steps_, 0);
  if (--closed_->open_steps_ == 0) {
    closed_->step_started_ = false;
    closed_->keep_redo_ = false;
  }
}

ClosedEntries::ClosedEntries() = default;
ClosedEntries::~ClosedEntries() = default;

void ClosedEntries::RecordRemoval(const SpaceModel& model,
                                  const SidebarNode& entry) {
  CHECK_NE(entry.type(), Type::kRoot);
  // A Today folder closes as its last entry leaves, and so on up
  // (SpaceModel's invariants): it comes back with the entry.
  const SidebarNode* node = &entry;
  while (node->parent()->type() == Type::kFolder &&
         node->parent()->children().size() == 1 &&
         model.today_root()->Contains(node->parent())) {
    node = node->parent();
  }

  Step& step = CurrentStep(model);
  if (!step.Contains(node->id())) {
    Entry closed;
    closed.id = node->id();
    closed.favorites = model.kind() == SpaceModel::Kind::kFavorites;
    closed.node = SerializeEntry(*node);
    CollectIds(*node, closed.ids);
    const SidebarNode* parent = node->parent();
    closed.parent_id = parent->id();
    closed.index = parent->GetIndexOf(node);
    if (closed.index > 0) {
      closed.previous_id = parent->children()[closed.index - 1]->id();
    }
    closed.in_today = model.today_root()->Contains(node);
    // A split of two ends as one pane leaves.
    if (parent->type() == Type::kSplit && parent->children().size() == 2) {
      closed.split_sibling_id =
          parent->children()[closed.index == 0 ? 1 : 0]->id();
    }
    step.entries.push_back(std::move(closed));
  }
  EndStep();
}

void ClosedEntries::RecordUnload(const SpaceModel& model,
                                 const SidebarNode& tab) {
  CHECK_EQ(tab.type(), Type::kTab);
  Step& step = CurrentStep(model);
  if (!step.Contains(tab.id())) {
    Entry closed;
    closed.id = tab.id();
    closed.favorites = model.kind() == SpaceModel::Kind::kFavorites;
    closed.ids = {tab.id()};
    closed.url = tab.url();
    closed.title = tab.title();
    step.entries.push_back(std::move(closed));
  }
  EndStep();
}

void ClosedEntries::ExpectClose(const base::Uuid& tab) {
  expected_closes_.insert(tab);
}

bool ClosedEntries::TakeExpectedClose(const base::Uuid& tab) {
  return expected_closes_.erase(tab) > 0;
}

void ClosedEntries::SetOpen(const base::Uuid& tab, bool shown) {
  Step* step = FindStep(tab);
  if (!step) {
    return;
  }
  if (step->reopen_was_shown) {
    // The tab the user was looking at. Tabs closing after it in the same
    // step were shown only as it closed.
    return;
  }
  if (shown) {
    step->reopen_id = tab;
    step->reopen_was_shown = true;
  } else if (!step->reopen_id.is_valid()) {
    step->reopen_id = tab;
  }
}

void ClosedEntries::SetHistory(const base::Uuid& tab, int32_t history) {
  if (Step* step = FindStep(tab)) {
    step->history[tab] = history;
  }
}

std::optional<ClosedEntries::Step> ClosedEntries::TakeUndo() {
  if (undo_.empty()) {
    return std::nullopt;
  }
  Step step = std::move(undo_.back());
  undo_.pop_back();
  // A tab whose close never came (beforeunload kept it) closes on its own
  // from now on.
  for (const Entry& entry : step.entries) {
    for (const base::Uuid& id : entry.ids) {
      expected_closes_.erase(id);
    }
  }
  return step;
}

void ClosedEntries::PushRedo(Step step) {
  redo_.push_back(std::move(step));
  if (redo_.size() > kMaxSteps) {
    redo_.erase(redo_.begin());
  }
}

std::optional<ClosedEntries::Step> ClosedEntries::TakeRedo() {
  if (redo_.empty()) {
    return std::nullopt;
  }
  Step step = std::move(redo_.back());
  redo_.pop_back();
  return step;
}

// static
SidebarNode* ClosedEntries::Restore(
    const Step& step,
    SpaceModel& model,
    SpaceModel& favorites,
    base::FunctionRef<bool(const SidebarNode&)> is_open) {
  for (const Entry& entry : step.entries) {
    SpaceModel& target = entry.favorites ? favorites : model;
    if (!entry.node) {
      // A tab that stayed: back to the page it showed, unless it's open
      // again, showing another.
      SidebarNode* tab = target.FindById(entry.id);
      if (tab && tab->type() == Type::kTab && entry.url.is_valid() &&
          !is_open(*tab)) {
        target.SetUrl(tab, entry.url);
        target.SetTitle(tab, entry.title);
      }
      continue;
    }
    if (!target.FindById(entry.id)) {
      RestoreRemoved(entry, target);
    }
  }
  if (!step.reopen_id.is_valid()) {
    return nullptr;
  }
  SidebarNode* reopen = model.FindById(step.reopen_id);
  if (!reopen) {
    reopen = favorites.FindById(step.reopen_id);
  }
  return reopen && reopen->type() == Type::kTab ? reopen : nullptr;
}

ClosedEntries::Step& ClosedEntries::CurrentStep(const SpaceModel& model) {
  if (!step_started_) {
    if (!keep_redo_) {
      redo_.clear();
    }
    undo_.emplace_back();
    if (undo_.size() > kMaxSteps) {
      undo_.erase(undo_.begin());
    }
    step_started_ = true;
  }
  Step& step = undo_.back();
  if (model.kind() == SpaceModel::Kind::kSpace && !step.space_id.is_valid()) {
    step.space_id = model.id();
  }
  return step;
}

void ClosedEntries::EndStep() {
  if (open_steps_ == 0) {
    step_started_ = false;
  }
}

ClosedEntries::Step* ClosedEntries::FindStep(const base::Uuid& id) {
  for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
    if (it->Contains(id)) {
      return &*it;
    }
  }
  return nullptr;
}

}  // namespace compass
