// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/space_list.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"

namespace compass {

SpaceList::SpaceList() {
  spaces_.push_back(std::make_unique<SpaceModel>());
  active_space_ = spaces_.front().get();
  active_space_->SetName(u"Home");
  active_space_->AddObserver(this);
}

SpaceList::~SpaceList() {
  for (const auto& space : spaces_) {
    space->RemoveObserver(this);
  }
  // |active_space_| points into |spaces_|.
  active_space_ = nullptr;
}

SpaceModel* SpaceList::FindSpace(const base::Uuid& id) const {
  for (const auto& space : spaces_) {
    if (space->id() == id) {
      return space.get();
    }
  }
  return nullptr;
}

SpaceModel* SpaceList::FindSpaceOf(const SidebarNode* node) const {
  for (const auto& space : spaces_) {
    if (space->Contains(node)) {
      return space.get();
    }
  }
  return nullptr;
}

SpaceModel* SpaceList::AddSpace(size_t index,
                                std::u16string name,
                                std::optional<base::Uuid> id) {
  CHECK(!id || !FindSpace(*id));
  auto space = std::make_unique<SpaceModel>();
  if (id) {
    space->set_id(*id);
  }
  space->SetName(std::move(name));
  SpaceModel* added =
      spaces_
          .insert(spaces_.begin() + std::min(index, spaces_.size()),
                  std::move(space))
          ->get();
  added->AddObserver(this);
  for (Observer& observer : observers_) {
    observer.OnSpaceAdded(*added);
  }
  return added;
}

void SpaceList::RemoveSpace(SpaceModel& space) {
  CHECK_GT(spaces_.size(), 1u);
  auto it =
      std::ranges::find(spaces_, &space, &std::unique_ptr<SpaceModel>::get);
  CHECK(it != spaces_.end());
  if (active_space_ == &space) {
    SetActiveSpace(it + 1 != spaces_.end() ? **(it + 1) : **(it - 1));
  }
  for (Observer& observer : observers_) {
    observer.OnSpaceWillBeRemoved(space);
  }
  space.RemoveObserver(this);
  // Observers may have changed the list.
  std::erase_if(spaces_,
                [&](const auto& entry) { return entry.get() == &space; });
  for (Observer& observer : observers_) {
    observer.OnSpaceRemoved();
  }
}

void SpaceList::MoveSpace(SpaceModel& space, size_t index) {
  auto it =
      std::ranges::find(spaces_, &space, &std::unique_ptr<SpaceModel>::get);
  CHECK(it != spaces_.end());
  const size_t from = it - spaces_.begin();
  index = std::min(index, spaces_.size());
  // The slots right before and after it leave it in place.
  if (index == from || index == from + 1) {
    return;
  }
  std::unique_ptr<SpaceModel> moved = std::move(*it);
  spaces_.erase(it);
  spaces_.insert(spaces_.begin() + (index > from ? index - 1 : index),
                 std::move(moved));
  for (Observer& observer : observers_) {
    observer.OnSpaceMoved(space);
  }
}

void SpaceList::SetActiveSpace(SpaceModel& space) {
  CHECK(FindSpace(space.id()) == &space);
  if (active_space_ == &space) {
    return;
  }
  active_space_ = &space;
  for (Observer& observer : observers_) {
    observer.OnActiveSpaceChanged();
  }
}

void SpaceList::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void SpaceList::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void SpaceList::OnSpaceChanged(const SpaceModel& space) {
  for (Observer& observer : observers_) {
    observer.OnSpaceUpdated(space);
  }
}

}  // namespace compass
