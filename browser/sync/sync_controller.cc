// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/sync_controller.h"

#include <utility>

#include "compass/strings/grit/compass_strings.h"
#include "ui/base/l10n/l10n_util.h"

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
                                       bool push_relay,
                                       StartCallback callback) {
  std::move(callback).Run(
      false, l10n_util::GetStringUTF8(IDS_COMPASS_SYNC_NOT_SUPPORTED), {});
}

void SyncController::StartOnCloudflareSignIn(bool push_relay,
                                             StartCallback callback) {
  std::move(callback).Run(
      false, l10n_util::GetStringUTF8(IDS_COMPASS_SYNC_NOT_SUPPORTED), {});
}

void SyncController::JoinWithInvite(const std::string& invite_link,
                                    StartCallback callback) {
  std::move(callback).Run(
      false, l10n_util::GetStringUTF8(IDS_COMPASS_SYNC_NOT_SUPPORTED), {});
}

void SyncController::GetServerPeople(PeopleCallback callback) {
  std::move(callback).Run(std::nullopt);
}

void SyncController::InviteFriend(const std::string& name,
                                  InviteCallback callback) {
  std::move(callback).Run(
      false, l10n_util::GetStringUTF8(IDS_COMPASS_SYNC_NOT_SUPPORTED),
      std::string());
}

void SyncController::GetServerUpdate(ServerUpdateCallback callback) {
  std::move(callback).Run(std::nullopt);
}

void SyncController::UpdateServer(const std::string& access_token,
                                  DoneCallback callback) {
  std::move(callback).Run(
      false, l10n_util::GetStringUTF8(IDS_COMPASS_SYNC_NOT_SUPPORTED));
}

void SyncController::SetServerPushRelay(bool on, DoneCallback callback) {
  std::move(callback).Run(
      false, l10n_util::GetStringUTF8(IDS_COMPASS_SYNC_NOT_SUPPORTED));
}

void SyncController::RemoveFromServer(const std::string& target,
                                      DoneCallback callback) {
  std::move(callback).Run(
      false, l10n_util::GetStringUTF8(IDS_COMPASS_SYNC_NOT_SUPPORTED));
}

void SyncController::SetProfileMapping(const ProfileMapping& mapping,
                                       DoneCallback callback) {
  std::move(callback).Run(
      false, l10n_util::GetStringUTF8(IDS_COMPASS_SYNC_MAPPING_NOT_NEEDED));
}

}  // namespace compass::sync
