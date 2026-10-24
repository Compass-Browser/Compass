// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_SIDEBAR_SPACE_LIST_H_
#define STRATA_BROWSER_SIDEBAR_SPACE_LIST_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/uuid.h"
#include "strata/browser/sidebar/space_model.h"

namespace strata {

// A profile's spaces, in the switcher's order, and the active one: the space
// new windows open in. Never empty. See docs/design/spaces.md.
class SpaceList {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnSpaceAdded(SpaceModel& space) {}
    virtual void OnActiveSpaceChanged() {}
  };

  // Starts with one space, "Home", which is active.
  SpaceList();
  SpaceList(const SpaceList&) = delete;
  SpaceList& operator=(const SpaceList&) = delete;
  ~SpaceList();

  const std::vector<std::unique_ptr<SpaceModel>>& spaces() const {
    return spaces_;
  }
  SpaceModel* FindSpace(const base::Uuid& id) const;
  // The space that holds |node|, or null.
  SpaceModel* FindSpaceOf(const SidebarNode* node) const;

  // Adds a space at |index| (clamped to the end). |id| is for spaces loaded
  // from disk; new spaces get a fresh one.
  SpaceModel* AddSpace(size_t index,
                       std::u16string name,
                       std::optional<base::Uuid> id = std::nullopt);

  SpaceModel& active_space() const { return *active_space_; }
  void SetActiveSpace(SpaceModel& space);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

 private:
  std::vector<std::unique_ptr<SpaceModel>> spaces_;
  raw_ptr<SpaceModel> active_space_;
  base::ObserverList<Observer> observers_;
};

}  // namespace strata

#endif  // STRATA_BROWSER_SIDEBAR_SPACE_LIST_H_
