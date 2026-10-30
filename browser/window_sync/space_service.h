// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_WINDOW_SYNC_SPACE_SERVICE_H_
#define COMPASS_BROWSER_WINDOW_SYNC_SPACE_SERVICE_H_

#include <memory>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/live_tab_registry.h"
#include "compass/browser/window_sync/restored_tab_matcher.h"

class BrowserWindowInterface;
class Profile;

namespace compass {

class SidebarStore;

// A profile's sidebar state, shared by all its windows: its spaces, which
// nodes are live where, the store that persists them (none for off-the-record
// profiles), a matcher per space that ties restored tabs to saved entries,
// the parking window that holds the live tabs of spaces no window shows, and
// the timer that auto-archives idle Today entries.
// Attached to the Profile as user data, so it outlives the profile's windows.
// See docs/design/sidebar.md, slice d, and docs/design/spaces.md.
class SpaceService : public base::SupportsUserData::Data,
                     public SpaceList::Observer {
 public:
  // Creates the parking window for a profile. Tests can't create real
  // windows, so they set their own.
  using ParkingWindowFactory =
      base::RepeatingCallback<BrowserWindowInterface*(Profile*)>;

  SpaceService(const SpaceService&) = delete;
  SpaceService& operator=(const SpaceService&) = delete;
  ~SpaceService() override;

  // Creates the service, and starts loading, on first use.
  static SpaceService* Get(Profile* profile);

  SpaceList& space_list() { return space_list_; }
  LiveTabRegistry& registry() { return registry_; }
  RestoredTabMatcher& restored_tab_matcher(const SpaceModel& space);
  SidebarStore* store() { return store_.get(); }
  // Whether its file has loaded (always, off the record).
  bool loaded() const { return loaded_; }
  // Runs |callback| once the file has loaded: now, if it has.
  void RunWhenLoaded(base::OnceClosure callback);

  // The entry |space| showed last, if it still exists.
  SidebarNode* GetLastActiveNode(SpaceModel& space);
  void SetLastActiveNode(const SpaceModel& space, const SidebarNode& node);

  // The parking window, created on first use. Never shown. Once a switch has
  // taken its last tab, upstream closes it, and the next one is a new window.
  BrowserWindowInterface* GetOrCreateParkingWindow();
  // Null if there's none, or it's closing.
  BrowserWindowInterface* parking_window() const;
  // Set while the parking window is being created, so its WindowFeatures
  // know what they're for.
  bool creating_parking_window() const { return creating_parking_window_; }
  bool IsParkingWindow(const BrowserWindowInterface* browser) const;
  // Called by a parking window's WindowFeatures as it goes away.
  void OnParkingWindowDestroyed(const BrowserWindowInterface* browser);
  // Closes the parking window, and with it the tabs it holds.
  void CloseParkingWindow();

  void SetParkingWindowFactoryForTesting(ParkingWindowFactory factory);

  // Archives idle Today entries now (see ArchiveIdleTodayEntries()). Also
  // runs on its own, shortly after the file loads and then periodically.
  void ArchiveIdleEntries();

  base::WeakPtr<SpaceService> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // SpaceList::Observer:
  void OnSpaceAdded(SpaceModel& space) override;
  void OnSpaceWillBeRemoved(SpaceModel& space) override;

 private:
  explicit SpaceService(Profile* profile);

  void OnLoaded();

  const raw_ptr<Profile> profile_;
  SpaceList space_list_;
  LiveTabRegistry registry_;
  // By space. Spaces stay put in memory, but the active space's ID can change
  // while the file loads.
  base::flat_map<const SpaceModel*, std::unique_ptr<RestoredTabMatcher>>
      matchers_;
  base::flat_map<const SpaceModel*, base::Uuid> last_active_nodes_;
  bool loaded_ = false;
  std::vector<base::OnceClosure> when_loaded_;
  base::OneShotTimer first_archive_timer_;
  base::RepeatingTimer archive_timer_;

  raw_ptr<BrowserWindowInterface> parking_window_ = nullptr;
  bool creating_parking_window_ = false;
  ParkingWindowFactory parking_window_factory_;

  // It observes the spaces, and flushes when destroyed.
  std::unique_ptr<SidebarStore> store_;

  base::WeakPtrFactory<SpaceService> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_WINDOW_SYNC_SPACE_SERVICE_H_
