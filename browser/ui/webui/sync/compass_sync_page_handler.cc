// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sync/compass_sync_page_handler.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/escape.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "compass/browser/sync/sync_settings.h"
#include "components/qr_code_generator/qr_code_generator.h"

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

sync::mojom::DataType DataTypeToMojo(sync::SyncDataType type) {
  switch (type) {
    case sync::SyncDataType::kPasswords:
      return sync::mojom::DataType::kPasswords;
    case sync::SyncDataType::kHistory:
      return sync::mojom::DataType::kHistory;
    case sync::SyncDataType::kBookmarks:
      return sync::mojom::DataType::kBookmarks;
    case sync::SyncDataType::kAutofill:
      return sync::mojom::DataType::kAutofill;
    case sync::SyncDataType::kExtensions:
      return sync::mojom::DataType::kExtensions;
    case sync::SyncDataType::kSettings:
      return sync::mojom::DataType::kSettings;
    case sync::SyncDataType::kReadingList:
      return sync::mojom::DataType::kReadingList;
    case sync::SyncDataType::kOpenTabs:
      return sync::mojom::DataType::kOpenTabs;
    case sync::SyncDataType::kSpaces:
      return sync::mojom::DataType::kSpaces;
  }
  return sync::mojom::DataType::kPasswords;
}

sync::SyncDataType DataTypeFromMojo(sync::mojom::DataType type) {
  switch (type) {
    case sync::mojom::DataType::kPasswords:
      return sync::SyncDataType::kPasswords;
    case sync::mojom::DataType::kHistory:
      return sync::SyncDataType::kHistory;
    case sync::mojom::DataType::kBookmarks:
      return sync::SyncDataType::kBookmarks;
    case sync::mojom::DataType::kAutofill:
      return sync::SyncDataType::kAutofill;
    case sync::mojom::DataType::kExtensions:
      return sync::SyncDataType::kExtensions;
    case sync::mojom::DataType::kSettings:
      return sync::SyncDataType::kSettings;
    case sync::mojom::DataType::kReadingList:
      return sync::SyncDataType::kReadingList;
    case sync::mojom::DataType::kOpenTabs:
      return sync::SyncDataType::kOpenTabs;
    case sync::mojom::DataType::kSpaces:
      return sync::SyncDataType::kSpaces;
  }
  return sync::SyncDataType::kPasswords;
}

}  // namespace

CompassSyncPageHandler::CompassSyncPageHandler(
    mojo::PendingReceiver<sync::mojom::PageHandler> receiver,
    mojo::PendingRemote<sync::mojom::Page> page,
    sync::SyncController* controller,
    Reauthenticator reauthenticator)
    : receiver_(this, std::move(receiver)),
      page_(std::move(page)),
      controller_(controller),
      reauthenticator_(std::move(reauthenticator)) {
  observation_.Observe(controller_);
}

CompassSyncPageHandler::~CompassSyncPageHandler() = default;

// static
sync::mojom::StatusPtr CompassSyncPageHandler::ToMojo(
    const sync::SyncStatus& status) {
  return sync::mojom::Status::New(
      StateToMojo(status.state), status.server_url, status.message,
      base::UTF16ToUTF8(sync::GetSyncMenuLabel(status)));
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
std::string CompassSyncPageHandler::PairingInviteUrl(
    const std::string& server_url,
    const std::string& code) {
  if (server_url.empty() || code.empty()) {
    return std::string();
  }
  return "compass-pair:?server=" +
         base::EscapeQueryParamValue(server_url, /*use_plus=*/false) +
         "&code=" + base::EscapeQueryParamValue(code, /*use_plus=*/false);
}

// static
sync::mojom::PairingPtr CompassSyncPageHandler::ToMojo(
    const sync::PairingStatus& pairing,
    const std::string& server_url) {
  auto result = sync::mojom::Pairing::New();
  result->step = StepToMojo(pairing.step);
  result->code = pairing.code;
  result->device_name = pairing.device_name;
  result->message = pairing.message;
  if (pairing.step != sync::PairingStep::kShowingCode) {
    return result;
  }
  result->invite_url = PairingInviteUrl(server_url, pairing.code);
  if (result->invite_url.empty()) {
    return result;
  }
  auto qr = qr_code_generator::GenerateCode(
      base::as_byte_span(result->invite_url));
  if (qr.has_value()) {
    result->qr.reserve(qr->data.size());
    for (uint8_t module : qr->data) {
      result->qr.push_back(module & 1);
    }
    result->qr_size = qr->qr_size;
  }
  return result;
}

// static
sync::mojom::DetailsPtr CompassSyncPageHandler::ToMojo(
    const std::vector<sync::SyncDevice>& devices,
    const std::vector<sync::SyncDataTypeState>& types) {
  auto details = sync::mojom::Details::New();
  for (const sync::SyncDevice& device : devices) {
    details->devices.push_back(sync::mojom::Device::New(
        device.name, device.kind,
        device.last_seen.is_null()
            ? 0.0
            : device.last_seen.InMillisecondsFSinceUnixEpochIgnoringNull(),
        device.this_device));
  }
  for (const sync::SyncDataTypeState& type : types) {
    details->types.push_back(sync::mojom::DataTypeState::New(
        DataTypeToMojo(type.type), type.enabled));
  }
  return details;
}

sync::mojom::DetailsPtr CompassSyncPageHandler::GetDetails() const {
  return ToMojo(controller_->GetDevices(), controller_->GetDataTypes());
}

sync::mojom::PairingPtr CompassSyncPageHandler::GetPairing() const {
  return ToMojo(controller_->GetPairingStatus(),
                controller_->GetStatus().server_url);
}

void CompassSyncPageHandler::GetState(GetStateCallback callback) {
  std::move(callback).Run(ToMojo(controller_->GetStatus()),
                          GetPairing(), GetDetails());
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

void CompassSyncPageHandler::SetDataTypeEnabled(sync::mojom::DataType type,
                                                bool enabled) {
  controller_->SetDataTypeEnabled(DataTypeFromMojo(type), enabled);
}

void CompassSyncPageHandler::RevealRecoveryCode(
    RevealRecoveryCodeCallback callback) {
  if (reauthenticating_) {
    std::move(callback).Run(false, "Already asking.", {});
    return;
  }
  if (controller_->GetRecoveryCode().empty()) {
    std::move(callback).Run(false, "Sync isn't running on this device.", {});
    return;
  }
  reauthenticating_ = true;
  reauthenticator_.Run(
      base::BindOnce(&CompassSyncPageHandler::OnReauthenticated,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void CompassSyncPageHandler::OnReauthenticated(
    RevealRecoveryCodeCallback callback,
    bool authenticated) {
  reauthenticating_ = false;
  if (!authenticated) {
    std::move(callback).Run(false, std::string(), {});
    return;
  }
  // Read now, not before: the chain may have changed meanwhile.
  std::vector<std::string> words = controller_->GetRecoveryCode();
  const bool ok = !words.empty();
  std::move(callback).Run(
      ok, ok ? std::string() : "Sync isn't running on this device.",
      std::move(words));
}

void CompassSyncPageHandler::ResetSync(ResetSyncCallback callback) {
  controller_->ResetChain(base::BindOnce(
      [](ResetSyncCallback callback, bool ok, std::string error,
         std::vector<std::string> words) {
        std::move(callback).Run(ok, error, words);
      },
      std::move(callback)));
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
  page_->OnStateChanged(ToMojo(controller_->GetStatus()), GetPairing(),
                        GetDetails());
}

}  // namespace compass
