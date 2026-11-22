// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The settings window's Profiles and Spaces panes: every profile and every
// space, whichever profile the window is (docs/design/settings-window.md).

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/delete_profile_helper.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_metrics.h"
#include "chrome/browser/profiles/profiles_state.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/ui/webui/settings/settings_window_handler.h"
#include "compass/browser/ui/webui/sidebar/space_mojo.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"

namespace compass {

namespace {

// The listed profiles, as the profile menu lists them.
std::vector<ProfileAttributesEntry*> GetListedProfiles() {
  std::vector<ProfileAttributesEntry*> profiles;
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (!profile_manager) {
    return profiles;
  }
  for (ProfileAttributesEntry* entry :
       profile_manager->GetProfileAttributesStorage()
           .GetAllProfilesAttributesSortedForDisplay()) {
    if (!entry->IsOmitted()) {
      profiles.push_back(entry);
    }
  }
  return profiles;
}

// The listed profile named |profile_id|, if there's one.
ProfileAttributesEntry* FindListedProfile(const std::string& profile_id) {
  std::optional<base::FilePath> path = ProfilePathFromId(profile_id);
  if (!path) {
    return nullptr;
  }
  ProfileAttributesEntry* entry = g_browser_process->profile_manager()
                                      ->GetProfileAttributesStorage()
                                      .GetProfileAttributesWithPath(*path);
  return entry && !entry->IsOmitted() ? entry : nullptr;
}

}  // namespace

SpaceDirectory* SettingsWindowHandler::directory() {
  // Gone with the profile manager, during shutdown.
  return uses_directory_ ? SpaceDirectory::GetIfExists() : nullptr;
}

std::vector<SpaceDirectory::Entry> SettingsWindowHandler::GetAllSpaces() {
  if (SpaceDirectory* directory = this->directory()) {
    return directory->GetSpaces();
  }
  std::vector<SpaceDirectory::Entry> entries;
  if (SpaceList* list = GetSpaceList()) {
    for (const auto& space : list->spaces()) {
      entries.push_back({profile_->GetOriginalProfile(), space.get()});
    }
  }
  return entries;
}

SpaceDirectory::Entry SettingsWindowHandler::FindAnySpace(
    const std::string& id) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  if (!uuid.is_valid()) {
    return {};
  }
  if (SpaceDirectory* directory = this->directory()) {
    return directory->FindSpace(uuid);
  }
  SpaceList* list = GetSpaceList();
  SpaceModel* space = list ? list->FindSpace(uuid) : nullptr;
  return space ? SpaceDirectory::Entry{profile_->GetOriginalProfile(), space}
               : SpaceDirectory::Entry();
}

SpaceModel* SettingsWindowHandler::FindEditableSpace(const std::string& id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  return entry.space && !IsViewOnlySpace(entry.profile, *entry.space)
             ? entry.space.get()
             : nullptr;
}

std::vector<sidebar::mojom::ProfileOptionPtr>
SettingsWindowHandler::GetSpaceProfileOptions() {
  // Spaces change profile only through the directory.
  return directory() ? GetProfileOptions()
                     : std::vector<sidebar::mojom::ProfileOptionPtr>();
}

// Profiles --------------------------------------------------------------------

std::vector<sidebar::mojom::ProfileDetailsPtr>
SettingsWindowHandler::DescribeProfiles() {
  std::map<base::FilePath, uint32_t> space_counts;
  for (const SpaceDirectory::Entry& entry : GetAllSpaces()) {
    ++space_counts[entry.profile->GetOriginalProfile()->GetPath()];
  }
  const base::FilePath own_path = profile_->GetOriginalProfile()->GetPath();
  std::vector<sidebar::mojom::ProfileDetailsPtr> profiles;
  for (ProfileAttributesEntry* entry : GetListedProfiles()) {
    auto details = sidebar::mojom::ProfileDetails::New();
    details->id = entry->GetPath().BaseName().AsUTF8Unsafe();
    details->name = base::UTF16ToUTF8(entry->GetLocalProfileName());
    details->current = entry->GetPath() == own_path;
    details->space_count = space_counts[entry->GetPath()];
    profiles.push_back(std::move(details));
  }
  return profiles;
}

void SettingsWindowHandler::SendProfilesAndSpaces() {
  page_->ProfilesChanged(DescribeProfiles());
  std::vector<sidebar::mojom::SpacePtr> spaces;
  for (const SpaceDirectory::Entry& entry : GetAllSpaces()) {
    spaces.push_back(ToMojoSpace(entry, profile_->GetOriginalProfile()));
  }
  page_->SpacesChanged(std::move(spaces), GetSpaceProfileOptions());
}

void SettingsWindowHandler::GetProfiles(GetProfilesCallback callback) {
  std::move(callback).Run(DescribeProfiles());
}

void SettingsWindowHandler::SetProfileName(const std::string& profile_id,
                                           const std::string& name) {
  std::u16string trimmed;
  base::TrimWhitespace(base::UTF8ToUTF16(name), base::TRIM_ALL, &trimmed);
  ProfileAttributesEntry* entry = FindListedProfile(profile_id);
  if (trimmed.empty() || !entry) {
    return;
  }
  // A loaded profile's name goes through upstream's path, which also renames
  // its shortcuts; an unloaded one's is only in the storage.
  if (Profile* profile = g_browser_process->profile_manager()->GetProfileByPath(
          entry->GetPath())) {
    profiles::UpdateProfileName(profile, trimmed);
  } else {
    entry->SetLocalProfileName(trimmed, /*is_default_name=*/false);
  }
}

void SettingsWindowHandler::CreateProfile() {
  // It joins the switcher with its starting space.
  CreateProfileForSpaces(base::BindOnce(&AddSpaceToProfile));
}

void SettingsWindowHandler::DeleteProfile(const std::string& profile_id) {
  ProfileAttributesEntry* entry = FindListedProfile(profile_id);
  if (!entry || GetListedProfiles().size() < 2) {
    return;
  }
  // Its windows close first, this one's too if it's the window's profile.
  g_browser_process->profile_manager()
      ->GetDeleteProfileHelper()
      .MaybeScheduleProfileForDeletion(entry->GetPath(), base::DoNothing(),
                                       ProfileMetrics::DELETE_PROFILE_SETTINGS);
}

void SettingsWindowHandler::OnProfileAdded(const base::FilePath& profile_path) {
  SendProfilesAndSpaces();
}

void SettingsWindowHandler::OnProfileWasRemoved(
    const base::FilePath& profile_path,
    const std::u16string& profile_name) {
  SendProfilesAndSpaces();
}

void SettingsWindowHandler::OnProfileNameChanged(
    const base::FilePath& profile_path,
    const std::u16string& old_profile_name) {
  // Spaces show their profile's name.
  SendProfilesAndSpaces();
}

void SettingsWindowHandler::OnProfileIsOmittedChanged(
    const base::FilePath& profile_path) {
  SendProfilesAndSpaces();
}

void SettingsWindowHandler::OnDirectoryChanged() {
  // Space counts change with the spaces.
  SendProfilesAndSpaces();
}

// Spaces ----------------------------------------------------------------------

void SettingsWindowHandler::GetSpaces(GetSpacesCallback callback) {
  std::vector<sidebar::mojom::SpacePtr> spaces;
  for (const SpaceDirectory::Entry& entry : GetAllSpaces()) {
    spaces.push_back(ToMojoSpace(entry, profile_->GetOriginalProfile()));
  }
  std::move(callback).Run(std::move(spaces), GetSpaceProfileOptions());
}

void SettingsWindowHandler::AddSpace(const std::string& profile_id) {
  ProfileAttributesEntry* entry = FindListedProfile(profile_id);
  if (!entry) {
    return;
  }
  if (!directory()) {
    // Only the window's own profile's spaces show.
    if (entry->GetPath() == profile_->GetOriginalProfile()->GetPath()) {
      AddSpaceToProfile(profile_->GetOriginalProfile());
    }
    return;
  }
  g_browser_process->profile_manager()->LoadProfileByPath(
      entry->GetPath(), /*incognito=*/false,
      base::BindOnce(&AddSpaceToProfile));
}

void SettingsWindowHandler::RenameSpace(const std::string& id,
                                        const std::string& name) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::RenameSpace(*space, name);
  }
}

void SettingsWindowHandler::SetSpaceColor(const std::string& id,
                                          sidebar::mojom::SpaceColorPtr color) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceColor(*space, color);
  }
}

void SettingsWindowHandler::SetSpaceGradient(
    const std::string& id,
    std::vector<sidebar::mojom::SpaceColorPtr> gradient) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceGradient(*space, gradient);
  }
}

void SettingsWindowHandler::SetSpaceIntensity(const std::string& id,
                                              double intensity) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceIntensity(*space, intensity);
  }
}

void SettingsWindowHandler::SetSpaceGrain(const std::string& id, double grain) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceGrain(*space, grain);
  }
}

void SettingsWindowHandler::SetSpaceAppearance(
    const std::string& id,
    sidebar::mojom::SpaceAppearance appearance) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceAppearance(*space, appearance);
  }
}

void SettingsWindowHandler::SetSpaceIcon(const std::string& id,
                                         sidebar::mojom::SpaceIconPtr icon) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceIcon(*space, icon);
  }
}

void SettingsWindowHandler::SetSpaceProfile(
    const std::string& id,
    const std::optional<std::string>& profile_id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  if (!entry.space || !directory()) {
    return;
  }
  std::optional<base::FilePath> path;
  if (profile_id) {
    path = ProfilePathFromId(*profile_id);
    if (!path) {
      return;
    }
  }
  AssignSpaceToProfile(entry.space->id(), std::move(path));
}

void SettingsWindowHandler::SetArchiveAfter(const std::string& space_id,
                                            uint32_t hours) {
  if (SpaceModel* space = FindAnySpace(space_id).space) {
    SetSpaceArchiveAfter(*space, hours);
  }
}

void SettingsWindowHandler::SetSpaceSynced(const std::string& space_id,
                                           bool synced) {
  if (SpaceModel* space = FindAnySpace(space_id).space) {
    space->SetSynced(synced);
  }
}

void SettingsWindowHandler::DeleteSpace(const std::string& id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  if (entry.space) {
    compass::DeleteSpace(entry.profile, *entry.space);
  }
}

void SettingsWindowHandler::MoveSpace(const std::string& id, uint32_t index) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  if (!entry.space) {
    return;
  }
  if (SpaceDirectory* directory = this->directory()) {
    directory->MoveSpace(*entry.space, index);
  } else if (SpaceList* list = GetSpaceList()) {
    list->MoveSpace(*entry.space, index);
  }
}

}  // namespace compass
