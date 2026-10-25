// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_WINDOW_SYNC_SPACE_DIRECTORY_H_
#define STRATA_BROWSER_WINDOW_SYNC_SPACE_DIRECTORY_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/scoped_observation.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile_manager_observer.h"
#include "strata/browser/sidebar/space_list.h"

class Profile;
class ProfileManager;
class ScopedProfileKeepAlive;

namespace strata {

class SpaceModel;
class SpaceService;

// The spaces of every profile the switcher lists, browser-wide. Loads those
// profiles on first use and keeps them loaded, so a space's parked tabs stay
// while no window of its profile is open. Off-the-record, guest and omitted
// profiles aren't listed. See docs/design/spaces.md, slice 3.
class SpaceDirectory : public ProfileManagerObserver, public SpaceList::Observer {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // A profile's spaces joined or left the directory, or one of them was
    // added, removed, renamed or recolored.
    virtual void OnDirectoryChanged() {}
  };

  // A space and the profile it belongs to.
  struct Entry {
    raw_ptr<Profile> profile = nullptr;
    raw_ptr<SpaceModel> space = nullptr;
  };

  SpaceDirectory(const SpaceDirectory&) = delete;
  SpaceDirectory& operator=(const SpaceDirectory&) = delete;
  ~SpaceDirectory() override;

  // Creates the directory on first use, which starts loading every listed
  // profile. Null without a profile manager, and once it's being destroyed.
  static SpaceDirectory* Get();
  // Doesn't create it.
  static SpaceDirectory* GetIfExists();

  // Whether |profile|'s spaces are listed.
  static bool IsListed(const Profile* profile);
  // |profile|'s name, as upstream's profile menu shows it.
  static std::u16string GetProfileName(const Profile* profile);

  // Every listed profile's spaces, in the switcher's order: profiles as
  // upstream's profile menu orders them, each profile's spaces in its order.
  std::vector<Entry> GetSpaces() const;
  // The space with |id| in a listed profile, or an empty entry.
  Entry FindSpace(const base::Uuid& id) const;

  // Called by the SpaceService of a listed profile as it's created and
  // destroyed.
  void OnServiceCreated(Profile* profile, SpaceService* service);
  void OnServiceDestroyed(SpaceService* service);
  // Called by a listed profile's SpaceService once its file has loaded.
  void OnServiceLoaded();

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // ProfileManagerObserver:
  void OnProfileAdded(Profile* profile) override;
  void OnProfileMarkedForPermanentDeletion(Profile* profile) override;
  void OnProfileManagerDestroying() override;

  // SpaceList::Observer:
  void OnSpaceAdded(SpaceModel& space) override;
  void OnSpaceUpdated(const SpaceModel& space) override;
  void OnSpaceRemoved() override;

 private:
  struct Member;

  SpaceDirectory();

  // Lists the loaded profiles, and loads the others.
  void AddProfiles();
  void NotifyChanged();

  // By profile path.
  std::map<base::FilePath, std::unique_ptr<Member>> members_;
  base::ObserverList<Observer> observers_;
  base::ScopedObservation<ProfileManager, ProfileManagerObserver>
      profile_manager_observation_{this};
  base::WeakPtrFactory<SpaceDirectory> weak_factory_{this};
};

}  // namespace strata

#endif  // STRATA_BROWSER_WINDOW_SYNC_SPACE_DIRECTORY_H_
