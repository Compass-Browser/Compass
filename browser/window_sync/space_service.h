// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_WINDOW_SYNC_SPACE_SERVICE_H_
#define STRATA_BROWSER_WINDOW_SYNC_SPACE_SERVICE_H_

#include <memory>

#include "base/supports_user_data.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/live_tab_registry.h"
#include "strata/browser/window_sync/restored_tab_matcher.h"

class Profile;

namespace strata {

class SidebarStore;

// A profile's sidebar state, shared by all its windows: the SpaceModel, which
// nodes are live where, the store that persists it (none for off-the-record
// profiles), and the matcher that ties restored tabs to saved entries. Attached to the Profile as user data, so it
// outlives the profile's windows. One space per profile until spaces arrive
// (ARCHITECTURE.md §6). See docs/design/sidebar.md, slice d.
class SpaceService : public base::SupportsUserData::Data {
 public:
  SpaceService(const SpaceService&) = delete;
  SpaceService& operator=(const SpaceService&) = delete;
  ~SpaceService() override;

  // Creates the service, and starts loading, on first use.
  static SpaceService* Get(Profile* profile);

  SpaceModel& space_model() { return space_model_; }
  LiveTabRegistry& registry() { return registry_; }
  RestoredTabMatcher& restored_tab_matcher() { return restored_tab_matcher_; }
  SidebarStore* store() { return store_.get(); }

 private:
  explicit SpaceService(Profile* profile);

  SpaceModel space_model_;
  LiveTabRegistry registry_;
  RestoredTabMatcher restored_tab_matcher_{&space_model_, &registry_};
  // Declared last: it observes the model, and flushes when destroyed.
  std::unique_ptr<SidebarStore> store_;
};

}  // namespace strata

#endif  // STRATA_BROWSER_WINDOW_SYNC_SPACE_SERVICE_H_
