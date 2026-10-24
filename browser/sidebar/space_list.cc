// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/sidebar/space_list.h"

#include <algorithm>
#include <utility>

#include "base/check.h"

namespace strata {

SpaceList::SpaceList() {
  spaces_.push_back(std::make_unique<SpaceModel>());
  active_space_ = spaces_.front().get();
  active_space_->SetName(u"Home");
}

SpaceList::~SpaceList() {
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
  for (Observer& observer : observers_) {
    observer.OnSpaceAdded(*added);
  }
  return added;
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

}  // namespace strata
