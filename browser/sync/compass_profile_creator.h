// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_COMPASS_PROFILE_CREATOR_H_
#define COMPASS_BROWSER_SYNC_COMPASS_PROFILE_CREATOR_H_

#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "compass/components/sync/profile_list.h"

class Profile;

namespace compass::sync {

// Where local sync would keep its file, in the profile's directory. Never
// written (the engine talks to the server instead), but local sync doesn't
// start on macOS without it.
inline constexpr base::FilePath::CharType kLocalSyncDirName[] =
    FILE_PATH_LITERAL("Compass Sync");

// Creates the chain's profiles a device doesn't have (slice 5), already in
// the chain: the new profile's directory starts with a Preferences file that
// has the chain (the seed as the home profile stores it, the server, the
// profile's UUID, its own account) and local sync on, so its sync service
// talks to the profile's account from the start, without a restart.
// Chromium's sync service reads the local-sync pref once, as the profile
// loads; a profile created first and set up afterwards would sync only after
// the next launch. Then the profile is listed in the switcher (which keeps it
// loaded), where its spaces arrive with its first sync.
class CompassProfileCreator : public MissingProfiles::Creator {
 public:
  // Runs for each profile created, with its sync UUID.
  using CreatedCallback =
      base::RepeatingCallback<void(const std::string& uuid, Profile* profile)>;

  // |encrypted_seed| is compass.sync.seed as the home profile has it (the
  // same os_crypt key serves every profile); |server_url| the chain's.
  CompassProfileCreator(std::string encrypted_seed,
                        std::string server_url,
                        CreatedCallback on_created);
  CompassProfileCreator(const CompassProfileCreator&) = delete;
  CompassProfileCreator& operator=(const CompassProfileCreator&) = delete;
  ~CompassProfileCreator() override;

  // The prefs a profile of the chain starts with: what CompassSyncService
  // and the sync service read at load. |profile_path| is its directory.
  static base::DictValue MakeInitialPrefs(const std::string& encrypted_seed,
                                          const std::string& server_url,
                                          const std::string& profile_uuid,
                                          const base::FilePath& profile_path);

  // MissingProfiles::Creator:
  void CreateProfile(const ChainProfile& profile,
                     base::OnceCallback<void(bool created)> done) override;

 private:
  void OnPrefsWritten(base::FilePath path,
                      std::string uuid,
                      base::OnceCallback<void(bool created)> done,
                      bool written);
  void OnProfileLoaded(std::string uuid,
                       base::OnceCallback<void(bool created)> done,
                       Profile* profile);

  const std::string encrypted_seed_;
  const std::string server_url_;
  const CreatedCallback on_created_;
  base::WeakPtrFactory<CompassProfileCreator> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_BROWSER_SYNC_COMPASS_PROFILE_CREATOR_H_
