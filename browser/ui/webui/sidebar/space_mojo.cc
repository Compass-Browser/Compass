// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sidebar/space_mojo.h"

#include <utility>

#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "compass/browser/rooms/room_model.h"
#include "compass/browser/rooms/room_service.h"
#include "compass/browser/sync/compass_sync_service.h"
#include "compass/browser/sync/compass_sync_service_factory.h"
#include "compass/browser/window_sync/space_service.h"
#include "third_party/skia/include/core/SkColor.h"

namespace compass {

// Pages cast between the two.
static_assert(static_cast<int>(sidebar::mojom::SpaceAppearance::kMaxValue) ==
              static_cast<int>(SpaceAppearance::kDark));

sidebar::mojom::SpaceColorPtr ToMojoColor(SpaceColor color) {
  if (color.is_grey()) {
    return nullptr;
  }
  const SkColor tint = *color.tint();
  return sidebar::mojom::SpaceColor::New(SkColorGetR(tint), SkColorGetG(tint),
                                         SkColorGetB(tint));
}

sidebar::mojom::SpaceIconPtr ToMojoIcon(const SpaceIcon& icon) {
  switch (icon.kind()) {
    case SpaceIcon::Kind::kNone:
      return nullptr;
    case SpaceIcon::Kind::kEmoji:
      return sidebar::mojom::SpaceIcon::NewEmoji(icon.value());
    case SpaceIcon::Kind::kSymbol:
      return sidebar::mojom::SpaceIcon::NewSymbol(icon.value());
  }
  NOTREACHED();
}

sidebar::mojom::SpacePtr ToMojoSpace(const SpaceDirectory::Entry& entry,
                                     const Profile* own_profile) {
  const SpaceModel* space = entry.space;
  auto mojo_space = sidebar::mojom::Space::New();
  mojo_space->profile_id =
      entry.profile->GetOriginalProfile()->GetBaseName().AsUTF8Unsafe();
  mojo_space->profile_name =
      base::UTF16ToUTF8(SpaceDirectory::GetProfileName(entry.profile));
  mojo_space->own_profile = entry.profile == own_profile;
  mojo_space->id = space->id().AsLowercaseString();
  mojo_space->name = base::UTF16ToUTF8(space->name());
  mojo_space->color = ToMojoColor(space->color());
  for (SpaceColor color : space->gradient()) {
    mojo_space->gradient.push_back(ToMojoColor(color));
  }
  mojo_space->intensity = space->intensity();
  mojo_space->grain = space->grain();
  mojo_space->appearance =
      static_cast<sidebar::mojom::SpaceAppearance>(space->appearance());
  mojo_space->icon = ToMojoIcon(space->icon());
  mojo_space->archive_after_hours =
      static_cast<uint32_t>(space->archive_after().IntDiv(GetArchiveHour()));
  mojo_space->synced = space->synced();
  sync::CompassSyncService* sync_service =
      sync::CompassSyncServiceFactory::GetForProfileIfExists(
          entry.profile->GetOriginalProfile());
  mojo_space->sync_on = sync_service && sync_service->AreDataTypesReleased();
  if (rooms::RoomService* rooms =
          entry.profile->IsOffTheRecord()
              ? nullptr
              : rooms::RoomService::GetIfExists(entry.profile)) {
    const rooms::SharedState state = rooms->GetSpaceState(mojo_space->id);
    mojo_space->can_share = rooms->IsAvailable();
    mojo_space->shared = state.shared;
    mojo_space->view_only = state.read_only;
  }
  return mojo_space;
}

std::vector<sidebar::mojom::ProfileOptionPtr> GetProfileOptions() {
  std::vector<sidebar::mojom::ProfileOptionPtr> profiles;
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (!profile_manager) {
    return profiles;
  }
  for (ProfileAttributesEntry* profile :
       profile_manager->GetProfileAttributesStorage()
           .GetAllProfilesAttributesSortedForDisplay()) {
    if (!profile->IsOmitted()) {
      profiles.push_back(sidebar::mojom::ProfileOption::New(
          profile->GetPath().BaseName().AsUTF8Unsafe(),
          base::UTF16ToUTF8(profile->GetName())));
    }
  }
  return profiles;
}

std::optional<base::FilePath> ProfilePathFromId(const std::string& id) {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  const base::FilePath name = base::FilePath::FromUTF8Unsafe(id);
  // A directory name, nothing more.
  if (!profile_manager || name.empty() || name.BaseName() != name ||
      name.ReferencesParent()) {
    return std::nullopt;
  }
  return profile_manager->user_data_dir().Append(name);
}

bool IsViewOnlySpace(Profile* profile, const SpaceModel& space) {
  rooms::RoomService* rooms = profile && !profile->IsOffTheRecord()
                                  ? rooms::RoomService::GetIfExists(profile)
                                  : nullptr;
  return rooms &&
         rooms->GetSpaceState(space.id().AsLowercaseString()).read_only;
}

void RenameSpace(SpaceModel& space, const std::string& name) {
  std::u16string name16;
  base::TrimWhitespace(base::UTF8ToUTF16(name), base::TRIM_ALL, &name16);
  if (!name16.empty()) {
    space.SetName(std::move(name16));
  }
}

void SetSpaceColor(SpaceModel& space,
                   const sidebar::mojom::SpaceColorPtr& color) {
  space.SetColor(
      color ? SpaceColor::FromRGB(color->red, color->green, color->blue)
            : SpaceColor::kGrey);
}

void SetSpaceGradient(
    SpaceModel& space,
    const std::vector<sidebar::mojom::SpaceColorPtr>& gradient) {
  std::vector<SpaceColor> colors;
  for (const sidebar::mojom::SpaceColorPtr& color : gradient) {
    colors.push_back(
        SpaceColor::FromRGB(color->red, color->green, color->blue));
  }
  space.SetGradient(std::move(colors));
}

void SetSpaceIntensity(SpaceModel& space, double intensity) {
  space.SetIntensity(intensity);
}

void SetSpaceGrain(SpaceModel& space, double grain) {
  space.SetGrain(grain);
}

void SetSpaceAppearance(SpaceModel& space,
                        sidebar::mojom::SpaceAppearance appearance) {
  // Mojo has validated |appearance|.
  space.SetAppearance(static_cast<SpaceAppearance>(appearance));
}

void SetSpaceIcon(SpaceModel& space, const sidebar::mojom::SpaceIconPtr& icon) {
  if (!icon) {
    space.SetIcon(SpaceIcon());
    return;
  }
  std::optional<SpaceIcon> valid = icon->is_emoji()
                                       ? SpaceIcon::Emoji(icon->get_emoji())
                                       : SpaceIcon::Symbol(icon->get_symbol());
  if (valid) {
    space.SetIcon(std::move(*valid));
  }
}

void SetSpaceArchiveAfter(SpaceModel& space, uint32_t hours) {
  if (hours <= kMaxArchiveAfterHours) {
    space.SetArchiveAfter(GetArchiveHour() * static_cast<int64_t>(hours));
  }
}

}  // namespace compass
