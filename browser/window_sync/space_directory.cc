// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/space_directory.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/keep_alive/profile_keep_alive_types.h"
#include "chrome/browser/profiles/keep_alive/scoped_profile_keep_alive.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/space_service.h"

namespace strata {

namespace {

SpaceDirectory* g_directory = nullptr;

}  // namespace

struct SpaceDirectory::Member {
  raw_ptr<Profile> profile;
  // Null until the profile's service is created.
  raw_ptr<SpaceService> service;
  std::unique_ptr<ScopedProfileKeepAlive> keep_alive;
};

SpaceDirectory::SpaceDirectory() = default;

SpaceDirectory::~SpaceDirectory() {
  for (auto& [path, member] : members_) {
    if (member->service) {
      member->service->space_list().RemoveObserver(this);
    }
  }
}

// static
SpaceDirectory* SpaceDirectory::Get() {
  if (g_directory || !g_browser_process) {
    return g_directory;
  }
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (!profile_manager) {
    return nullptr;
  }
  g_directory = new SpaceDirectory();
  g_directory->profile_manager_observation_.Observe(profile_manager);
  // Not now: the first caller may be a service being constructed, which
  // OnProfileAdded() would otherwise construct again.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&SpaceDirectory::AddProfiles,
                                g_directory->weak_factory_.GetWeakPtr()));
  return g_directory;
}

// static
SpaceDirectory* SpaceDirectory::GetIfExists() {
  return g_directory;
}

// static
bool SpaceDirectory::IsListed(const Profile* profile) {
  if (profile->IsOffTheRecord() || profile->IsGuestSession() ||
      profile->IsSystemProfile()) {
    return false;
  }
  ProfileManager* profile_manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  ProfileAttributesEntry* entry =
      profile_manager ? profile_manager->GetProfileAttributesStorage()
                            .GetProfileAttributesWithPath(profile->GetPath())
                      : nullptr;
  return !entry || !entry->IsOmitted();
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

std::vector<SpaceDirectory::Entry> SpaceDirectory::GetSpaces() const {
  // The profile menu's order; profiles it doesn't know (in tests) last.
  std::vector<const Member*> ordered;
  if (ProfileManager* profile_manager = g_browser_process->profile_manager()) {
    for (ProfileAttributesEntry* entry :
         profile_manager->GetProfileAttributesStorage()
             .GetAllProfilesAttributesSortedForDisplay()) {
      auto it = members_.find(entry->GetPath());
      if (it != members_.end()) {
        ordered.push_back(it->second.get());
      }
    }
  }
  for (const auto& [path, member] : members_) {
    if (!std::ranges::contains(ordered, member.get())) {
      ordered.push_back(member.get());
    }
  }
  std::vector<Entry> spaces;
  for (const Member* member : ordered) {
    if (!member->service) {
      continue;
    }
    for (const auto& space : member->service->space_list().spaces()) {
      spaces.push_back({member->profile, space.get()});
    }
  }
  return spaces;
}

SpaceDirectory::Entry SpaceDirectory::FindSpace(const base::Uuid& id) const {
  for (const auto& [path, member] : members_) {
    if (!member->service) {
      continue;
    }
    if (SpaceModel* space = member->service->space_list().FindSpace(id)) {
      return {member->profile, space};
    }
  }
  return {};
}

void SpaceDirectory::AddProfiles() {
  ProfileManager* profile_manager =
      profile_manager_observation_.GetSource();
  for (Profile* profile : profile_manager->GetLoadedProfiles()) {
    OnProfileAdded(profile);
  }
  // The others report through OnProfileAdded() once loaded.
  for (ProfileAttributesEntry* entry :
       profile_manager->GetProfileAttributesStorage()
           .GetAllProfilesAttributes()) {
    if (!entry->IsOmitted() &&
        !profile_manager->GetProfileByPath(entry->GetPath())) {
      profile_manager->LoadProfileByPath(entry->GetPath(), /*incognito=*/false,
                                         base::DoNothing());
    }
  }
}

void SpaceDirectory::OnServiceCreated(Profile* profile, SpaceService* service) {
  std::unique_ptr<Member>& member = members_[profile->GetPath()];
  if (!member) {
    member = std::make_unique<Member>();
    member->profile = profile;
    // Null for a profile already on its way out.
    member->keep_alive = ScopedProfileKeepAlive::TryAcquire(
        profile, ProfileKeepAliveOrigin::kStrataSpaces);
  }
  member->service = service;
  service->space_list().AddObserver(this);
  NotifyChanged();
}

void SpaceDirectory::OnServiceDestroyed(SpaceService* service) {
  for (auto it = members_.begin(); it != members_.end(); ++it) {
    if (it->second->service == service) {
      service->space_list().RemoveObserver(this);
      // The profile is going: its keep-alive is moot.
      members_.erase(it);
      NotifyChanged();
      return;
    }
  }
}

void SpaceDirectory::OnServiceLoaded() {
  // The provisional space took its saved ID, and the saved spaces came in.
  NotifyChanged();
}

void SpaceDirectory::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void SpaceDirectory::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void SpaceDirectory::OnProfileAdded(Profile* profile) {
  if (IsListed(profile) && !members_.contains(profile->GetPath())) {
    // Registers itself through OnServiceCreated().
    SpaceService::Get(profile);
  }
}

void SpaceDirectory::OnProfileMarkedForPermanentDeletion(Profile* profile) {
  auto it = members_.find(profile->GetPath());
  if (it == members_.end()) {
    return;
  }
  if (it->second->service) {
    it->second->service->space_list().RemoveObserver(this);
  }
  members_.erase(it);
  NotifyChanged();
}

void SpaceDirectory::OnProfileManagerDestroying() {
  // The browser process may no longer return the manager going away.
  profile_manager_observation_.Reset();
  g_directory = nullptr;
  delete this;
}

void SpaceDirectory::OnSpaceAdded(SpaceModel& space) {
  NotifyChanged();
}

void SpaceDirectory::OnSpaceUpdated(const SpaceModel& space) {
  NotifyChanged();
}

void SpaceDirectory::OnSpaceRemoved() {
  NotifyChanged();
}

void SpaceDirectory::NotifyChanged() {
  for (Observer& observer : observers_) {
    observer.OnDirectoryChanged();
  }
}

}  // namespace strata
