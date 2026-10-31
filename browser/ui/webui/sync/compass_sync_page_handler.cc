// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sync/compass_sync_page_handler.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"

namespace compass {
namespace {

sync::mojom::State StateToMojo(sync::SyncState state) {
  switch (state) {
    case sync::SyncState::kOff:
      return sync::mojom::State::kOff;
    case sync::SyncState::kNeedsRestart:
      return sync::mojom::State::kNeedsRestart;
    case sync::SyncState::kStarting:
      return sync::mojom::State::kStarting;
    case sync::SyncState::kOn:
      return sync::mojom::State::kOn;
    case sync::SyncState::kError:
      return sync::mojom::State::kError;
  }
  return sync::mojom::State::kError;
}

sync::mojom::PairingStep StepToMojo(sync::PairingStep step) {
  switch (step) {
    case sync::PairingStep::kNone:
      return sync::mojom::PairingStep::kNone;
    case sync::PairingStep::kWorking:
      return sync::mojom::PairingStep::kWorking;
    case sync::PairingStep::kShowingCode:
      return sync::mojom::PairingStep::kShowingCode;
    case sync::PairingStep::kConfirm:
      return sync::mojom::PairingStep::kConfirm;
    case sync::PairingStep::kWaitingForApproval:
      return sync::mojom::PairingStep::kWaitingForApproval;
    case sync::PairingStep::kDone:
      return sync::mojom::PairingStep::kDone;
    case sync::PairingStep::kFailed:
      return sync::mojom::PairingStep::kFailed;
  }
  return sync::mojom::PairingStep::kFailed;
}

}  // namespace

CompassSyncPageHandler::CompassSyncPageHandler(
    mojo::PendingReceiver<sync::mojom::PageHandler> receiver,
    mojo::PendingRemote<sync::mojom::Page> page,
    sync::SyncController* controller)
    : receiver_(this, std::move(receiver)),
      page_(std::move(page)),
      controller_(controller) {
  observation_.Observe(controller_);
}

CompassSyncPageHandler::~CompassSyncPageHandler() = default;

// static
sync::mojom::StatusPtr CompassSyncPageHandler::ToMojo(
    const sync::SyncStatus& status) {
  return sync::mojom::Status::New(StateToMojo(status.state), status.server_url,
                                  status.message);
}

// static
sync::mojom::ProfileMappingPtr CompassSyncPageHandler::ToMojo(
    const sync::ProfileMappingStatus& mapping) {
  auto result = sync::mojom::ProfileMapping::New();
  result->needed = mapping.needed;
  for (const sync::ChainProfile& profile : mapping.chain) {
    result->chain.push_back(sync::mojom::ChainProfile::New(
        profile.uuid, profile.name, profile.home));
  }
  for (const sync::LocalProfile& profile : mapping.local) {
    result->local.push_back(
        sync::mojom::LocalProfile::New(profile.id, profile.name));
  }
  for (const sync::ProfileMappingEntry& entry : mapping.proposed) {
    result->proposed.push_back(sync::mojom::ProfileMappingEntry::New(
        entry.local_id, entry.chain_uuid));
  }
  return result;
}

// static
sync::mojom::PairingPtr CompassSyncPageHandler::ToMojo(
    const sync::PairingStatus& pairing) {
  return sync::mojom::Pairing::New(StepToMojo(pairing.step), pairing.code,
                                   pairing.device_name, pairing.message);
}

void CompassSyncPageHandler::GetState(GetStateCallback callback) {
  std::move(callback).Run(ToMojo(controller_->GetStatus()),
                          ToMojo(controller_->GetPairingStatus()));
}

void CompassSyncPageHandler::StartSync(const std::string& server_url,
                                       StartSyncCallback callback) {
  controller_->StartNewChain(
      server_url, base::BindOnce(
                      [](StartSyncCallback callback, bool ok, std::string error,
                         std::vector<std::string> words) {
                        std::move(callback).Run(ok, error, words);
                      },
                      std::move(callback)));
}

void CompassSyncPageHandler::JoinWithRecoveryCode(
    const std::string& server_url,
    const std::string& words,
    JoinWithRecoveryCodeCallback callback) {
  controller_->JoinWithRecoveryCode(
      server_url, words,
      base::BindOnce(
          [](JoinWithRecoveryCodeCallback callback, bool ok,
             std::string error) { std::move(callback).Run(ok, error); },
          std::move(callback)));
}

void CompassSyncPageHandler::JoinWithPairingCode(const std::string& server_url,
                                                 const std::string& code) {
  controller_->JoinWithPairingCode(server_url, code);
}

void CompassSyncPageHandler::AddDevice() {
  controller_->AddDevice();
}

void CompassSyncPageHandler::AnswerPairing(bool allow) {
  controller_->AnswerPairing(allow);
}

void CompassSyncPageHandler::CancelPairing() {
  controller_->CancelPairing();
}

void CompassSyncPageHandler::Leave() {
  controller_->Leave();
}

void CompassSyncPageHandler::Restart() {
  controller_->Restart();
}

void CompassSyncPageHandler::GetProfileMapping(
    GetProfileMappingCallback callback) {
  std::move(callback).Run(ToMojo(controller_->GetProfileMapping()));
}

void CompassSyncPageHandler::SetProfileMapping(
    std::vector<sync::mojom::ProfileMappingEntryPtr> mapping,
    SetProfileMappingCallback callback) {
  sync::ProfileMapping entries;
  for (const sync::mojom::ProfileMappingEntryPtr& entry : mapping) {
    entries.push_back({entry->local_id, entry->chain_uuid});
  }
  controller_->SetProfileMapping(
      entries,
      base::BindOnce(
          [](SetProfileMappingCallback callback, bool ok, std::string error) {
            std::move(callback).Run(ok, error);
          },
          std::move(callback)));
}

void CompassSyncPageHandler::OnSyncControllerChanged() {
  page_->OnStateChanged(ToMojo(controller_->GetStatus()),
                        ToMojo(controller_->GetPairingStatus()));
}

}  // namespace compass
