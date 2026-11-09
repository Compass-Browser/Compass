// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// SpaceDirectory on Android (docs/design/android.md): the desktop's keeps the
// space switcher's order across profiles; Android has one profile, so there
// is no directory, and CompassSyncService's uses of it find none.

#include "compass/browser/window_sync/space_directory.h"

#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"

namespace compass {

// static
SpaceDirectory* SpaceDirectory::Get() {
  return nullptr;
}

// static
SpaceDirectory* SpaceDirectory::GetIfExists() {
  return nullptr;
}

// static
bool SpaceDirectory::IsListable(const Profile* profile) {
  return !profile->IsOffTheRecord() && !profile->IsGuestSession() &&
         !profile->IsSystemProfile();
}

// static
std::u16string SpaceDirectory::GetProfileName(const Profile* profile) {
  ProfileManager* profile_manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  ProfileAttributesEntry* entry =
      profile_manager ? profile_manager->GetProfileAttributesStorage()
                            .GetProfileAttributesWithPath(
                                profile->GetOriginalProfile()->GetPath())
                      : nullptr;
  return entry ? entry->GetName() : std::u16string();
}

// There's never a directory to call these on; they're here for the linker.
bool SpaceDirectory::IsListed(const Profile* profile) const {
  return false;
}

void SpaceDirectory::AddObserver(Observer* observer) {}

void SpaceDirectory::RemoveObserver(Observer* observer) {}

}  // namespace compass
