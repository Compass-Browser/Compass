// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SIDEBAR_SPACE_LIST_H_
#define COMPASS_BROWSER_SIDEBAR_SPACE_LIST_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/uuid.h"
#include "compass/browser/sidebar/space_model.h"

namespace compass {

// A profile's spaces, in the switcher's order, and the active one: the space
// new windows open in. Never empty. Also holds the profile's Favorites. See docs/design/spaces.md.
class SpaceList : public SpaceModel::Observer {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnSpaceAdded(SpaceModel& space) {}
    // |space|'s name or color changed.
    virtual void OnSpaceUpdated(const SpaceModel& space) {}
    // Called before |space| is destroyed. It's no longer active.
    virtual void OnSpaceWillBeRemoved(SpaceModel& space) {}
    // A space was removed and destroyed.
    virtual void OnSpaceRemoved() {}
    // |space| moved to another place in the list.
    virtual void OnSpaceMoved(const SpaceModel& space) {}
    virtual void OnActiveSpaceChanged() {}
  };

  // Starts with one space, "Home", which is active.
  SpaceList();
  SpaceList(const SpaceList&) = delete;
  SpaceList& operator=(const SpaceList&) = delete;
  ~SpaceList() override;

  const std::vector<std::unique_ptr<SpaceModel>>& spaces() const {
    return spaces_;
  }
  SpaceModel* FindSpace(const base::Uuid& id) const;
  // The space that holds |node|, or null. Never the favorites.
  SpaceModel* FindSpaceOf(const SidebarNode* node) const;

  // Adds a space at |index| (clamped to the end). |id| is for spaces loaded
  // from disk; new spaces get a fresh one.
  SpaceModel* AddSpace(size_t index,
                       std::u16string name,
                       std::optional<base::Uuid> id = std::nullopt);

  // Removes and destroys |space|, which mustn't be the only one. If it's
  // active, the space after it becomes active (the one before, if it's last).
  void RemoveSpace(SpaceModel& space);

  // Moves |space| to the slot |index| among the spaces, counted with it still
  // in place (clamped to the end).
  void MoveSpace(SpaceModel& space, size_t index);

  SpaceModel& active_space() const { return *active_space_; }

  // The profile's Favorites, shared by its spaces (see SpaceModel::Kind).
  SpaceModel& favorites() { return favorites_; }
  const SpaceModel& favorites() const { return favorites_; }
  void SetActiveSpace(SpaceModel& space);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // SpaceModel::Observer:
  void OnSpaceChanged(const SpaceModel& space) override;

 private:
  SpaceModel favorites_{SpaceModel::Kind::kFavorites};
  std::vector<std::unique_ptr<SpaceModel>> spaces_;
  raw_ptr<SpaceModel> active_space_;
  base::ObserverList<Observer> observers_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_SIDEBAR_SPACE_LIST_H_
