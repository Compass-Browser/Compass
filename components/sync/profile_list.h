// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_PROFILE_LIST_H_
#define COMPASS_COMPONENTS_SYNC_PROFILE_LIST_H_

#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"

// The chain's profiles (docs/design/sync.md, "Profiles" and "Slice 5
// notes"): each syncs to its own server account; the home profile's
// COMPASS_SPACES lists them, and pairing hands the list to the joining
// device so it can map them to its own profiles before the first sync.
namespace compass::sync {

// A profile of the chain, as the home profile's COMPASS_SPACES has it
// (CompassProfile) and pairing's message 4 carries it.
struct ChainProfile {
  // Its sync UUID (lowercase): the account is HKDF(seed, "compass-sync-auth:"
  // + uuid), except for the home profile's.
  std::string uuid;
  std::string name;
  // Chromium's default avatar icon index; -1 if not set.
  int avatar_index = -1;
  // The home profile: its account is the chain's own (no UUID in the
  // derivations), and it carries this list.
  bool home = false;

  bool operator==(const ChainProfile& other) const = default;
};

// Pairing's "profiles": [{"uuid": "...", "name": "...", "avatar": 26,
// "home": true}, ...] ("avatar" left out when not set).
base::ListValue ChainProfilesToValue(const std::vector<ChainProfile>& profiles);
// Skips entries without a valid lowercase UUID, and repeats of a UUID.
std::vector<ChainProfile> ChainProfilesFromValue(const base::ListValue& list);

// A profile of this device that can sync: one the switcher lists.
struct LocalProfile {
  // Identifies it on this device: its directory's name ("Profile 1").
  std::string id;
  // Its sync UUID (compass.sync.profile_uuid).
  std::string uuid;
  std::string name;
  int avatar_index = -1;

  bool operator==(const LocalProfile& other) const = default;
};

// What one local profile does as this device joins the chain: takes over the
// chain profile |chain_uuid| (it syncs with that profile's account and takes
// its UUID), or, with |chain_uuid| empty, joins the chain as a new profile.
struct ProfileMappingEntry {
  std::string local_id;
  std::string chain_uuid;

  bool operator==(const ProfileMappingEntry& other) const = default;
};
using ProfileMapping = std::vector<ProfileMappingEntry>;

// The proposal the join flow shows. The joining profile takes the home
// profile and isn't in |locals|. Each local profile, in order, takes the
// first chain profile not taken yet (never the home) with the same name,
// ignoring ASCII case and surrounding white space; else it joins as new.
ProfileMapping DefaultProfileMapping(const std::vector<ChainProfile>& chain,
                                     const std::vector<LocalProfile>& locals);

// Whether the user may choose |mapping|: exactly one entry per local profile,
// chain profiles of |chain| only, never the home, each at most once.
bool IsValidProfileMapping(const ProfileMapping& mapping,
                           const std::vector<ChainProfile>& chain,
                           const std::vector<LocalProfile>& locals);

// The chain's profiles no local profile takes, the home left out: this device
// creates them.
std::vector<ChainProfile> ProfilesToCreate(
    const std::vector<ChainProfile>& chain,
    const ProfileMapping& mapping);

// Creates the chain's profiles this device doesn't have yet, each once.
class MissingProfiles {
 public:
  class Creator {
   public:
    virtual ~Creator() = default;
    // Creates a profile for |profile| that syncs with its account (it takes
    // the UUID), and lists it in the switcher. |done| says whether it did.
    virtual void CreateProfile(const ChainProfile& profile,
                               base::OnceCallback<void(bool created)> done) = 0;
  };

  // |creator| must outlive |this|.
  explicit MissingProfiles(Creator* creator);
  MissingProfiles(const MissingProfiles&) = delete;
  MissingProfiles& operator=(const MissingProfiles&) = delete;
  ~MissingProfiles();

  // |chain| is the chain's list, |local_uuids| the sync UUIDs of this
  // device's profiles. Creates each chain profile (not the home) no local
  // one has, unless it's being created, or failed to be during this run.
  void Update(const std::vector<ChainProfile>& chain,
              const std::set<std::string>& local_uuids);

  bool IsCreating() const { return !creating_.empty(); }

 private:
  void OnCreated(const std::string& uuid, bool created);

  const raw_ptr<Creator> creator_;
  std::set<std::string> creating_;
  std::set<std::string> failed_;
  base::WeakPtrFactory<MissingProfiles> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_PROFILE_LIST_H_
