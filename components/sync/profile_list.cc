// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/profile_list.h"

#include <algorithm>
#include <map>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/uuid.h"

namespace compass::sync {
namespace {

constexpr char kUuidKey[] = "uuid";
constexpr char kNameKey[] = "name";
constexpr char kAvatarKey[] = "avatar";
constexpr char kHomeKey[] = "home";

bool SameName(const std::string& a, const std::string& b) {
  return base::EqualsCaseInsensitiveASCII(
      base::TrimWhitespaceASCII(a, base::TRIM_ALL),
      base::TrimWhitespaceASCII(b, base::TRIM_ALL));
}

}  // namespace

base::ListValue ChainProfilesToValue(
    const std::vector<ChainProfile>& profiles) {
  base::ListValue list;
  for (const ChainProfile& profile : profiles) {
    base::DictValue entry;
    entry.Set(kUuidKey, profile.uuid);
    entry.Set(kNameKey, profile.name);
    if (profile.avatar_index >= 0) {
      entry.Set(kAvatarKey, profile.avatar_index);
    }
    if (profile.home) {
      entry.Set(kHomeKey, true);
    }
    list.Append(std::move(entry));
  }
  return list;
}

std::vector<ChainProfile> ChainProfilesFromValue(const base::ListValue& list) {
  std::vector<ChainProfile> profiles;
  std::set<std::string> seen;
  for (const base::Value& value : list) {
    const base::DictValue* entry = value.GetIfDict();
    const std::string* uuid = entry ? entry->FindString(kUuidKey) : nullptr;
    if (!uuid || !base::Uuid::ParseLowercase(*uuid).is_valid() ||
        !seen.insert(*uuid).second) {
      continue;
    }
    ChainProfile profile;
    profile.uuid = *uuid;
    if (const std::string* name = entry->FindString(kNameKey)) {
      profile.name = *name;
    }
    profile.avatar_index = entry->FindInt(kAvatarKey).value_or(-1);
    if (profile.avatar_index < 0) {
      profile.avatar_index = -1;
    }
    profile.home = entry->FindBool(kHomeKey).value_or(false);
    profiles.push_back(std::move(profile));
  }
  return profiles;
}

ProfileMapping DefaultProfileMapping(const std::vector<ChainProfile>& chain,
                                     const std::vector<LocalProfile>& locals) {
  ProfileMapping mapping;
  std::set<std::string> taken;
  for (const LocalProfile& local : locals) {
    ProfileMappingEntry entry{local.id, std::string()};
    for (const ChainProfile& profile : chain) {
      if (!profile.home && !taken.contains(profile.uuid) &&
          SameName(profile.name, local.name)) {
        entry.chain_uuid = profile.uuid;
        taken.insert(profile.uuid);
        break;
      }
    }
    mapping.push_back(std::move(entry));
  }
  return mapping;
}

bool IsValidProfileMapping(const ProfileMapping& mapping,
                           const std::vector<ChainProfile>& chain,
                           const std::vector<LocalProfile>& locals) {
  if (mapping.size() != locals.size()) {
    return false;
  }
  std::set<std::string> local_ids;
  for (const LocalProfile& local : locals) {
    local_ids.insert(local.id);
  }
  std::map<std::string, bool> chain_home;
  for (const ChainProfile& profile : chain) {
    chain_home[profile.uuid] = profile.home;
  }
  std::set<std::string> seen_locals;
  std::set<std::string> taken;
  for (const ProfileMappingEntry& entry : mapping) {
    if (!local_ids.contains(entry.local_id) ||
        !seen_locals.insert(entry.local_id).second) {
      return false;
    }
    if (entry.chain_uuid.empty()) {
      continue;
    }
    auto it = chain_home.find(entry.chain_uuid);
    if (it == chain_home.end() || it->second ||
        !taken.insert(entry.chain_uuid).second) {
      return false;
    }
  }
  return true;
}

std::vector<ChainProfile> ProfilesToCreate(
    const std::vector<ChainProfile>& chain,
    const ProfileMapping& mapping) {
  std::set<std::string> taken;
  for (const ProfileMappingEntry& entry : mapping) {
    if (!entry.chain_uuid.empty()) {
      taken.insert(entry.chain_uuid);
    }
  }
  std::vector<ChainProfile> missing;
  for (const ChainProfile& profile : chain) {
    if (!profile.home && !taken.contains(profile.uuid)) {
      missing.push_back(profile);
    }
  }
  return missing;
}

MissingProfiles::MissingProfiles(Creator* creator) : creator_(creator) {}

MissingProfiles::~MissingProfiles() = default;

void MissingProfiles::Update(const std::vector<ChainProfile>& chain,
                             const std::set<std::string>& local_uuids) {
  for (const ChainProfile& profile : chain) {
    if (profile.home || local_uuids.contains(profile.uuid) ||
        creating_.contains(profile.uuid) || failed_.contains(profile.uuid)) {
      continue;
    }
    creating_.insert(profile.uuid);
    creator_->CreateProfile(
        profile, base::BindOnce(&MissingProfiles::OnCreated,
                                weak_factory_.GetWeakPtr(), profile.uuid));
  }
}

void MissingProfiles::OnCreated(const std::string& uuid, bool created) {
  creating_.erase(uuid);
  if (!created) {
    failed_.insert(uuid);
  }
}

}  // namespace compass::sync
