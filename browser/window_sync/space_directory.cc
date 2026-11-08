// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/window_sync/space_directory.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/keep_alive/profile_keep_alive_types.h"
#include "chrome/browser/profiles/keep_alive/scoped_profile_keep_alive.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/sidebar_store.h"
#include "compass/browser/window_sync/space_service.h"

namespace compass {

namespace {

SpaceDirectory* g_directory = nullptr;

constexpr base::FilePath::CharType kFileName[] =
    FILE_PATH_LITERAL("Compass Spaces.json");
constexpr base::TimeDelta kCommitInterval = base::Seconds(1);

// {"version": 1, "profiles": [<profile directory name>], "order": [<id>]}
constexpr char kVersionKey[] = "version";
constexpr char kProfilesKey[] = "profiles";
constexpr char kOrderKey[] = "order";

}  // namespace

SpaceDirectory::Member::Member() = default;
SpaceDirectory::Member::~Member() = default;

SpaceDirectory::SpaceDirectory(ProfileManager* profile_manager)
    : user_data_dir_(profile_manager->user_data_dir()),
      writer_(user_data_dir_.Append(kFileName),
              base::ThreadPool::CreateSequencedTaskRunner(
                  {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
                   base::TaskShutdownBehavior::BLOCK_SHUTDOWN}),
              kCommitInterval,
              "CompassSpaces") {
  profile_manager_observation_.Observe(profile_manager);
  RenamePlaceholderProfiles(profile_manager->GetProfileAttributesStorage());
}

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
  g_directory = new SpaceDirectory(profile_manager);
  // Answers later, so the first caller (a service being constructed) is done
  // by then.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadJsonFile, g_directory->writer_.path()),
      base::BindOnce(&SpaceDirectory::OnFileLoaded,
                     g_directory->weak_factory_.GetWeakPtr()));
  return g_directory;
}

// static
SpaceDirectory* SpaceDirectory::GetIfExists() {
  return g_directory;
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

std::vector<SpaceDirectory::Entry> SpaceDirectory::GetSpaces() const {
  std::vector<Entry> spaces;
  for (const auto& [path, member] : members_) {
    if (!member->service) {
      continue;
    }
    for (const auto& space : member->service->space_list().spaces()) {
      spaces.push_back({member->profile, space.get()});
    }
  }
  auto position = [&](const Entry& entry) {
    return std::ranges::find(order_, entry.space->id()) - order_.begin();
  };
  std::ranges::stable_sort(spaces, {}, position);
  return spaces;
}

bool SpaceDirectory::IsLoaded() const {
  if (!file_loaded_) {
    return false;
  }
  return std::ranges::all_of(listed_, [&](const base::FilePath& path) {
    auto it = members_.find(path);
    return it != members_.end() && it->second->service &&
           it->second->service->loaded();
  });
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

bool SpaceDirectory::IsListed(const Profile* profile) const {
  return listed_.contains(profile->GetPath());
}

void SpaceDirectory::List(Profile* profile) {
  if (!IsListable(profile) || IsListed(profile)) {
    return;
  }
  listed_.insert(profile->GetPath());
  Attach(profile);
  Changed();
}

void SpaceDirectory::Unlist(Profile* profile) {
  if (!listed_.erase(profile->GetPath())) {
    return;
  }
  auto it = members_.find(profile->GetPath());
  if (it != members_.end() && it->second->service) {
    for (const auto& space : it->second->service->space_list().spaces()) {
      if (!HasOtherSpaceWithId(*space)) {
        std::erase(order_, space->id());
      }
    }
  }
  Detach(profile->GetPath());
  Changed();
}

void SpaceDirectory::PlaceAfter(const SpaceModel& space,
                                const SpaceModel& anchor) {
  std::erase(order_, space.id());
  auto it = std::ranges::find(order_, anchor.id());
  order_.insert(it == order_.end() ? it : it + 1, space.id());
  Changed();
}

void SpaceDirectory::MoveSpace(const SpaceModel& space, size_t index) {
  std::vector<base::Uuid> shown;
  for (const Entry& entry : GetSpaces()) {
    shown.push_back(entry.space->id());
  }
  auto it = std::ranges::find(shown, space.id());
  if (it == shown.end()) {
    return;
  }
  const size_t from = it - shown.begin();
  index = std::min(index, shown.size());
  if (index == from || index == from + 1) {
    return;
  }
  // Every shown space gets a place, so the anchor below has one.
  for (const base::Uuid& id : shown) {
    if (!std::ranges::contains(order_, id)) {
      order_.push_back(id);
    }
  }
  std::erase(order_, space.id());
  // Before the space now in that slot, or after the last one shown. Spaces of
  // profiles not loaded keep their places among the others.
  if (index < shown.size()) {
    order_.insert(std::ranges::find(order_, shown[index]), space.id());
  } else {
    order_.insert(std::ranges::find(order_, shown.back()) + 1, space.id());
  }
  Changed();
}

void SpaceDirectory::OnServiceCreated(Profile* profile, SpaceService* service) {
  if (IsListed(profile) && !members_.contains(profile->GetPath())) {
    Attach(profile, service);
    Changed();
  }
}

void SpaceDirectory::OnServiceLoaded(SpaceService* service) {
  for (auto& [path, member] : members_) {
    if (member->service == service) {
      // The provisional space took its saved ID, and the saved spaces came in.
      AppendSpaces(service);
      Changed();
      return;
    }
  }
}

void SpaceDirectory::OnServiceDestroyed(SpaceService* service) {
  for (auto& [path, member] : members_) {
    if (member->service == service) {
      // The profile is going. It stays listed, for the next run.
      Detach(base::FilePath(path));
      Changed();
      return;
    }
  }
}

void SpaceDirectory::Flush() {
  if (writer_.HasPendingWrite()) {
    writer_.DoScheduledWrite();
  }
}

void SpaceDirectory::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void SpaceDirectory::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void SpaceDirectory::OnProfileAdded(Profile* profile) {
  if (IsListed(profile) && !members_.contains(profile->GetPath())) {
    Attach(profile);
    Changed();
  }
}

void SpaceDirectory::OnProfileMarkedForPermanentDeletion(Profile* profile) {
  Unlist(profile);
}

void SpaceDirectory::OnProfileManagerDestroying() {
  Flush();
  // The browser process may no longer return the manager going away.
  profile_manager_observation_.Reset();
  g_directory = nullptr;
  delete this;
}

void SpaceDirectory::OnSpaceAdded(SpaceModel& space) {
  for (auto& [path, member] : members_) {
    if (member->service &&
        member->service->space_list().FindSpace(space.id()) == &space) {
      // Spaces a file brings in are placed once it has loaded.
      if (member->service->loaded() &&
          !std::ranges::contains(order_, space.id())) {
        order_.push_back(space.id());
      }
      break;
    }
  }
  Changed();
}

void SpaceDirectory::OnSpaceUpdated(const SpaceModel& space) {
  Changed();
}

void SpaceDirectory::OnSpaceWillBeRemoved(SpaceModel& space) {
  // A space moving to another profile keeps its ID, and its copy there its
  // place.
  if (!HasOtherSpaceWithId(space)) {
    std::erase(order_, space.id());
  }
}

bool SpaceDirectory::HasOtherSpaceWithId(const SpaceModel& space) const {
  for (const auto& [path, member] : members_) {
    SpaceModel* found =
        member->service ? member->service->space_list().FindSpace(space.id())
                        : nullptr;
    if (found && found != &space) {
      return true;
    }
  }
  return false;
}

void SpaceDirectory::OnSpaceRemoved() {
  Changed();
}

std::optional<std::string> SpaceDirectory::SerializeData() {
  base::ListValue profiles;
  for (const base::FilePath& path : listed_) {
    if (path.DirName() == user_data_dir_) {
      profiles.Append(path.BaseName().AsUTF8Unsafe());
    }
  }
  base::ListValue order;
  for (const base::Uuid& id : order_) {
    order.Append(id.AsLowercaseString());
  }
  return base::WriteJsonWithOptions(
      base::DictValue()
          .Set(kVersionKey, 1)
          .Set(kProfilesKey, std::move(profiles))
          .Set(kOrderKey, std::move(order)),
      base::JSONWriter::OPTIONS_PRETTY_PRINT);
}

void SpaceDirectory::OnFileLoaded(std::optional<base::Value> data) {
  std::vector<base::Uuid> order;
  if (data && data->is_dict()) {
    if (const base::ListValue* profiles =
            data->GetDict().FindList(kProfilesKey)) {
      for (const base::Value& name : *profiles) {
        if (name.is_string()) {
          listed_.insert(user_data_dir_.Append(
              base::FilePath::FromUTF8Unsafe(name.GetString())));
        }
      }
    }
    if (const base::ListValue* ids = data->GetDict().FindList(kOrderKey)) {
      for (const base::Value& id : *ids) {
        base::Uuid uuid = id.is_string()
                              ? base::Uuid::ParseLowercase(id.GetString())
                              : base::Uuid();
        if (uuid.is_valid() && !std::ranges::contains(order, uuid)) {
          order.push_back(uuid);
        }
      }
    }
  }
  // Spaces placed before the file came in go after the saved ones.
  for (const base::Uuid& id : order_) {
    if (!std::ranges::contains(order, id)) {
      order.push_back(id);
    }
  }
  order_ = std::move(order);
  file_loaded_ = true;
  LoadListedProfiles();
  Changed();
}

void SpaceDirectory::LoadListedProfiles() {
  ProfileManager* profile_manager = profile_manager_observation_.GetSource();
  if (!profile_manager) {
    return;
  }
  for (auto it = listed_.begin(); it != listed_.end();) {
    if (Profile* profile = profile_manager->GetProfileByPath(*it)) {
      Attach(profile);
      ++it;
    } else if (profile_manager->LoadProfileByPath(*it, /*incognito=*/false,
                                                  base::DoNothing())) {
      // OnProfileAdded() attaches it.
      ++it;
    } else {
      // Deleted since.
      it = listed_.erase(it);
    }
  }
}

void SpaceDirectory::Attach(Profile* profile, SpaceService* service) {
  std::unique_ptr<Member>& member = members_[profile->GetPath()];
  if (member) {
    return;
  }
  member = std::make_unique<Member>();
  member->profile = profile;
  // Null for a profile already on its way out.
  member->keep_alive = ScopedProfileKeepAlive::TryAcquire(
      profile, ProfileKeepAliveOrigin::kCompassSpaces);
  // A service under construction passes itself: Get() would make another.
  member->service = service ? service : SpaceService::Get(profile);
  member->service->space_list().AddObserver(this);
  if (member->service->loaded()) {
    AppendSpaces(member->service);
  }
}

void SpaceDirectory::Detach(const base::FilePath& path) {
  auto it = members_.find(path);
  if (it == members_.end()) {
    return;
  }
  if (it->second->service) {
    it->second->service->space_list().RemoveObserver(this);
  }
  members_.erase(it);
}

void SpaceDirectory::AppendSpaces(SpaceService* service) {
  for (const auto& space : service->space_list().spaces()) {
    if (!std::ranges::contains(order_, space->id())) {
      order_.push_back(space->id());
    }
  }
}

void SpaceDirectory::DropStaleOrder() {
  if (stale_order_dropped_ || !IsLoaded()) {
    return;
  }
  stale_order_dropped_ = true;
  std::set<base::Uuid> ids;
  for (const auto& [path, member] : members_) {
    if (!member->service) {
      continue;
    }
    for (const auto& space : member->service->space_list().spaces()) {
      ids.insert(space->id());
    }
  }
  std::erase_if(order_,
                [&](const base::Uuid& id) { return !ids.contains(id); });
}

void SpaceDirectory::Changed() {
  DropStaleOrder();
  if (file_loaded_) {
    writer_.ScheduleWrite(this);
  }
  for (Observer& observer : observers_) {
    observer.OnDirectoryChanged();
  }
}

bool IsPlaceholderProfileName(std::u16string_view name) {
  return name == u"Your Chromium" || name == u"Your Compass";
}

void RenamePlaceholderProfiles(ProfileAttributesStorage& storage) {
  for (ProfileAttributesEntry* entry : storage.GetAllProfilesAttributes()) {
    if (IsPlaceholderProfileName(entry->GetLocalProfileName())) {
      entry->SetLocalProfileName(storage.ChooseNameForNewProfile(),
                                 /*is_default_name=*/true);
    }
  }
}

}  // namespace compass
