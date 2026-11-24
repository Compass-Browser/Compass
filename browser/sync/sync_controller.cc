// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/sync_controller.h"

#include <utility>

namespace compass::sync {

ProfileMappingStatus::ProfileMappingStatus() = default;
ProfileMappingStatus::ProfileMappingStatus(const ProfileMappingStatus&) =
    default;
ProfileMappingStatus& ProfileMappingStatus::operator=(
    const ProfileMappingStatus&) = default;
ProfileMappingStatus::~ProfileMappingStatus() = default;

ProfileMappingStatus SyncController::GetProfileMapping() const {
  return ProfileMappingStatus();
}

void SyncController::StartOnCloudflare(const std::string& access_token,
                                       StartCallback callback) {
  std::move(callback).Run(false, "Not supported here.", {});
}

void SyncController::StartOnCloudflareSignIn(StartCallback callback) {
  std::move(callback).Run(false, "Not supported here.", {});
}

void SyncController::JoinWithInvite(const std::string& invite_link,
                                    StartCallback callback) {
  std::move(callback).Run(false, "Not supported here.", {});
}

void SyncController::GetServerPeople(PeopleCallback callback) {
  std::move(callback).Run(std::nullopt);
}

void SyncController::InviteFriend(const std::string& name,
                                  InviteCallback callback) {
  std::move(callback).Run(false, "Not supported here.", std::string());
}

void SyncController::RemoveFromServer(const std::string& target,
                                      DoneCallback callback) {
  std::move(callback).Run(false, "Not supported here.");
}

void SyncController::SetProfileMapping(const ProfileMapping& mapping,
                                       DoneCallback callback) {
  std::move(callback).Run(false, "Nothing to map.");
}

}  // namespace compass::sync
