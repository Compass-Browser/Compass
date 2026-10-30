// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_WINDOW_SYNC_SPACE_DIRECTORY_H_
#define STRATA_BROWSER_WINDOW_SYNC_SPACE_DIRECTORY_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/important_file_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/scoped_observation.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile_manager_observer.h"
#include "strata/browser/sidebar/space_list.h"

class Profile;
class ProfileManager;
class ScopedProfileKeepAlive;

namespace strata {

class SpaceModel;
class SpaceService;

// The switcher's spaces, browser-wide, as in Arc: one list in the user's
// order, each space belonging to a profile. A space's entries live in its
// profile's SpaceList and file; this keeps the order and which profiles have
// spaces (listed profiles), in "Strata Spaces.json" in the user data
// directory. Listed profiles are loaded on first use and kept loaded, so a
// space's parked tabs stay while no window of its profile is open. A profile
// is listed once a window of it opens, or a space is assigned to it, and stops
// being listed when its last space goes. Off-the-record, guest and system
// profiles are never listed. See docs/design/spaces.md, slice 3.
class SpaceDirectory : public ProfileManagerObserver,
                       public SpaceList::Observer,
                       public base::ImportantFileWriter::DataSerializer {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // The list changed: a space was added, removed, moved, renamed or
    // recolored, or changed profile.
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

  // Creates the directory on first use, which starts loading its file and
  // then the listed profiles. Null without a profile manager. Goes with the
  // profile manager.
  static SpaceDirectory* Get();
  // Doesn't create it.
  static SpaceDirectory* GetIfExists();

  // Whether |profile| can have spaces in the list: not off-the-record, guest
  // or system profiles.
  static bool IsListable(const Profile* profile);
  // |profile|'s name, as upstream's profile menu shows it.
  static std::u16string GetProfileName(const Profile* profile);

  // The listed profiles' spaces, in the user's order.
  std::vector<Entry> GetSpaces() const;
  // The listed space with |id|, or an empty entry.
  Entry FindSpace(const base::Uuid& id) const;

  bool IsListed(const Profile* profile) const;
  // Lists |profile|'s spaces, at the end, if it can be listed.
  void List(Profile* profile);
  // Stops listing |profile|'s spaces. For a profile whose last space went.
  void Unlist(Profile* profile);

  // Puts |space| right after |anchor| in the list.
  void PlaceAfter(const SpaceModel& space, const SpaceModel& anchor);
  // Moves |space| to the slot |index| among GetSpaces(), counted with it
  // still in place (clamped to the end). Its profile's SpaceList keeps its own
  // order.
  void MoveSpace(const SpaceModel& space, size_t index);

  // Called by every SpaceService of a listable profile as it's created,
  // finishes loading its file, and is destroyed.
  void OnServiceCreated(Profile* profile, SpaceService* service);
  void OnServiceLoaded(SpaceService* service);
  void OnServiceDestroyed(SpaceService* service);

  // Writes pending changes now.
  void Flush();

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // ProfileManagerObserver:
  void OnProfileAdded(Profile* profile) override;
  void OnProfileMarkedForPermanentDeletion(Profile* profile) override;
  void OnProfileManagerDestroying() override;

  // SpaceList::Observer:
  void OnSpaceAdded(SpaceModel& space) override;
  void OnSpaceUpdated(const SpaceModel& space) override;
  void OnSpaceWillBeRemoved(SpaceModel& space) override;
  void OnSpaceRemoved() override;

  // base::ImportantFileWriter::DataSerializer:
  std::optional<std::string> SerializeData() override;

 private:
  // A listed, loaded profile.
  struct Member {
    Member();
    ~Member();
    raw_ptr<Profile> profile;
    // Null until the profile's service is created.
    raw_ptr<SpaceService> service;
    std::unique_ptr<ScopedProfileKeepAlive> keep_alive;
  };

  explicit SpaceDirectory(ProfileManager* profile_manager);

  void OnFileLoaded(std::optional<base::Value> data);
  // Loads the listed profiles that aren't, and attaches the loaded ones.
  void LoadListedProfiles();
  // Keeps a listed, loaded |profile| loaded and follows its spaces. A
  // service under construction passes itself.
  void Attach(Profile* profile, SpaceService* service = nullptr);
  void Detach(const base::FilePath& path);
  // Adds the spaces of |service| missing from the order, at the end.
  void AppendSpaces(SpaceService* service);
  void Changed();

  const base::FilePath user_data_dir_;
  // Listed profiles by path, loaded or not.
  std::set<base::FilePath> listed_;
  // The loaded ones.
  std::map<base::FilePath, std::unique_ptr<Member>> members_;
  // Space IDs in the user's order. Spaces missing from it go last.
  std::vector<base::Uuid> order_;
  bool file_loaded_ = false;

  base::ImportantFileWriter writer_;
  base::ObserverList<Observer> observers_;
  base::ScopedObservation<ProfileManager, ProfileManagerObserver>
      profile_manager_observation_{this};
  base::WeakPtrFactory<SpaceDirectory> weak_factory_{this};
};

}  // namespace strata

#endif  // STRATA_BROWSER_WINDOW_SYNC_SPACE_DIRECTORY_H_
