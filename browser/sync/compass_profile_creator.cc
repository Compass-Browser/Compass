// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/compass_profile_creator.h"

#include <optional>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_init_params.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_avatar_icon_util.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/chrome_constants.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/components/sync/pref_names.h"
#include "components/sync/base/pref_names.h"

namespace compass::sync {
namespace {

// Writes |prefs| as the new profile's Preferences, unless the directory
// already has one (then it isn't new: leave it).
bool WriteInitialPrefs(const base::FilePath& path, const std::string& json) {
  const base::FilePath file = path.Append(chrome::kPreferencesFilename);
  if (base::PathExists(file)) {
    return false;
  }
  return base::CreateDirectory(path) && base::WriteFile(file, json);
}

}  // namespace

CompassProfileCreator::CompassProfileCreator(std::string encrypted_seed,
                                             std::string server_url,
                                             CreatedCallback on_created)
    : encrypted_seed_(std::move(encrypted_seed)),
      server_url_(std::move(server_url)),
      on_created_(std::move(on_created)) {}

CompassProfileCreator::~CompassProfileCreator() = default;

// static
base::DictValue CompassProfileCreator::MakeInitialPrefs(
    const std::string& encrypted_seed,
    const std::string& server_url,
    const std::string& profile_uuid,
    const base::FilePath& profile_path) {
  // Pref names are dotted paths into the Preferences file.
  base::DictValue initial;
  initial.SetByDottedPath(prefs::kSeed, encrypted_seed);
  initial.SetByDottedPath(prefs::kServerUrl, server_url);
  initial.SetByDottedPath(prefs::kProfileUuid, profile_uuid);
  initial.SetByDottedPath(prefs::kProfileAccount, true);
  initial.SetByDottedPath(syncer::prefs::kEnableLocalSyncBackend, true);
  initial.SetByDottedPath(
      syncer::prefs::kLocalSyncBackendDir,
      profile_path.Append(kLocalSyncDirName).AsUTF8Unsafe());
  return initial;
}

void CompassProfileCreator::CreateProfile(
    const ChainProfile& profile,
    base::OnceCallback<void(bool created)> done) {
  ProfileManager* profile_manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  if (!profile_manager || encrypted_seed_.empty()) {
    std::move(done).Run(false);
    return;
  }
  ProfileAttributesStorage& storage =
      profile_manager->GetProfileAttributesStorage();
  // As ProfileManager::CreateMultiProfileAsync(): a fresh directory, with
  // the name and avatar in the storage before the profile loads.
  base::FilePath path;
  do {
    path = profile_manager->GenerateNextProfileDirectoryPath();
  } while (storage.GetProfileAttributesWithPath(path));
  ProfileAttributesInitParams params;
  params.profile_path = path;
  params.profile_name = profile.name.empty()
                            ? storage.ChooseNameForNewProfile()
                            : base::UTF8ToUTF16(profile.name);
  params.icon_index =
      profile.avatar_index >= 0 &&
              profiles::IsDefaultAvatarIconIndex(profile.avatar_index)
          ? static_cast<size_t>(profile.avatar_index)
          : storage.ChooseAvatarIconIndexForNewProfile();
  storage.AddProfile(std::move(params));

  std::optional<std::string> json = base::WriteJson(
      MakeInitialPrefs(encrypted_seed_, server_url_, profile.uuid, path));
  if (!json) {
    std::move(done).Run(false);
    return;
  }
  VLOG(1) << "[compass-sync] creating profile \"" << profile.name << "\" ("
          << profile.uuid << ") at " << path.BaseName().AsUTF8Unsafe();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&WriteInitialPrefs, path, std::move(*json)),
      base::BindOnce(&CompassProfileCreator::OnPrefsWritten,
                     weak_factory_.GetWeakPtr(), path, profile.uuid,
                     std::move(done)));
}

void CompassProfileCreator::OnPrefsWritten(
    base::FilePath path,
    std::string uuid,
    base::OnceCallback<void(bool created)> done,
    bool written) {
  ProfileManager* profile_manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  if (!written || !profile_manager) {
    LOG(ERROR) << "Compass sync: couldn't set up a new profile at "
               << path.AsUTF8Unsafe();
    std::move(done).Run(false);
    return;
  }
  profile_manager->CreateProfileAsync(
      path, base::BindOnce(&CompassProfileCreator::OnProfileLoaded,
                           weak_factory_.GetWeakPtr(), std::move(uuid),
                           std::move(done)));
}

void CompassProfileCreator::OnProfileLoaded(
    std::string uuid,
    base::OnceCallback<void(bool created)> done,
    Profile* profile) {
  SpaceDirectory* directory = SpaceDirectory::Get();
  if (!profile || !directory) {
    std::move(done).Run(false);
    return;
  }
  // Known as the chain's profile before the switcher lists it (listing makes
  // the home profile look for profiles to put in the chain).
  on_created_.Run(uuid, profile);
  // Listed: kept loaded, and in the switcher.
  directory->List(profile);
  std::move(done).Run(true);
}

}  // namespace compass::sync
