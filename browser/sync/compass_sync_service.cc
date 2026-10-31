// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/compass_sync_service.h"

#include <algorithm>
#include <utility>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/i18n/time_formatting.h"
#include "base/logging.h"
#include "base/power_monitor/power_monitor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/components/sync/pairing_network_transport.h"
#include "compass/components/sync/pref_names.h"
#include "compass/components/sync/sync_credentials.h"
#include "compass/components/sync/sync_hooks.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/sync/base/data_type.h"
#include "components/sync/base/passphrase_enums.h"
#include "components/sync/base/pref_names.h"
#include "components/sync/base/user_selectable_type.h"
#include "components/sync/service/sync_service.h"
#include "components/sync/service/sync_token_status.h"
#include "components/sync/service/sync_user_settings.h"
#include "components/sync_device_info/local_device_info_util.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace compass::sync {
namespace {

// Where local sync would keep its file. Never written (the engine talks to
// the server instead), but local sync doesn't start on macOS without it.
constexpr base::FilePath::CharType kLocalSyncDir[] =
    FILE_PATH_LITERAL("Compass Sync");

// Focus and wake ask the server for changes at most this often.
constexpr base::TimeDelta kMinRefreshInterval = base::Seconds(10);

// The types docs/design/sync.md, "What syncs", turns off that have a
// selectable type. WebAuthn credentials ride with Passwords, so their
// controller is left out instead (patches/
// chrome-browser-sync-sync_service_factory.cc.patch). History (kHistory:
// HISTORY and HISTORY_DELETE_DIRECTIVES) is wanted and selected like the rest;
// it runs under local sync, encrypted, since slice 2b (sync.md, "Slice 2b
// notes"). Its USER_EVENTS and JOURNEY stay off: local sync leaves them out.
constexpr syncer::UserSelectableType kUnwantedTypes[] = {
    syncer::UserSelectableType::kApps,
    syncer::UserSelectableType::kPayments,
    syncer::UserSelectableType::kProductComparison,
    syncer::UserSelectableType::kSavedTabGroups,
    syncer::UserSelectableType::kThemes,
};

std::string HostResultMessage(PairingHost::Result result) {
  switch (result) {
    case PairingHost::Result::kAdded:
      return "Device added. It starts syncing after a restart.";
    case PairingHost::Result::kDenied:
      return "Not added.";
    case PairingHost::Result::kWrongCode:
      return "The code didn't match. Try again with a new code.";
    case PairingHost::Result::kExpired:
      return "The code expired. Try again with a new code.";
    case PairingHost::Result::kRateLimited:
      return "The server is busy. Try again in a few minutes.";
    case PairingHost::Result::kFailed:
      return "Couldn't reach the server.";
  }
  return std::string();
}

std::string JoinResultMessage(PairingJoiner::Result result) {
  switch (result) {
    case PairingJoiner::Result::kJoined:
      return "Joined. Restart Compass to start syncing.";
    case PairingJoiner::Result::kDenied:
      return "The other device didn't add this one.";
    case PairingJoiner::Result::kBadCode:
      return "Enter the code as shown, like 7-482913.";
    case PairingJoiner::Result::kNoSuchCode:
      return "No such code on this server, or it expired.";
    case PairingJoiner::Result::kCodeUsed:
      return "That code was already used. Ask for a new one.";
    case PairingJoiner::Result::kWrongCode:
      return "The code didn't match. Ask for a new one.";
    case PairingJoiner::Result::kClosed:
      return "The other device ended pairing. Check the code and ask for a "
             "new one.";
    case PairingJoiner::Result::kExpired:
      return "The code expired. Ask for a new one.";
    case PairingJoiner::Result::kRateLimited:
      return "Too many attempts. Try again in a few minutes.";
    case PairingJoiner::Result::kFailed:
      return "Couldn't reach the server.";
  }
  return std::string();
}

}  // namespace

CompassSyncService::CompassSyncService(
    Profile* profile,
    syncer::SyncService* sync_service,
    os_crypt_async::OSCryptAsync* os_crypt_async)
    : profile_(profile),
      prefs_(profile->GetPrefs()),
      sync_service_(sync_service),
      os_crypt_async_(os_crypt_async),
      credentials_(SyncCredentials::ForPrefs(profile->GetPrefs())) {
  if (sync_service_) {
    sync_observation_.Observe(sync_service_);
  }
  if (ProfileBrowserCollection* browsers =
          ProfileBrowserCollection::GetForProfile(profile)) {
    browser_observation_.Observe(browsers);
  }
  if (base::PowerMonitor* power = base::PowerMonitor::GetInstance()) {
    power->AddPowerSuspendObserver(this);
    observing_power_ = true;
  }
  if (IsChainRunning()) {
    // No data type is selected, so nothing is committed, until the
    // passphrase is in place (UpdatePassphrase()). Nigori still configures,
    // which is how the passphrase state becomes known. (A setup-in-progress
    // handle would hold Nigori back too, and the two would wait on each
    // other.)
    HoldTypes();
    LoadSeed();
  }
}

CompassSyncService::~CompassSyncService() = default;

// static
void CompassSyncService::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterStringPref(prefs::kServerUrl, std::string());
  registry->RegisterStringPref(prefs::kSeed, std::string());
}

void CompassSyncService::Shutdown() {
  host_.reset();
  joiner_.reset();
  setup_handle_.reset();
  sync_observation_.Reset();
  browser_observation_.Reset();
  if (observing_power_) {
    base::PowerMonitor::GetInstance()->RemovePowerSuspendObserver(this);
    observing_power_ = false;
  }
  sync_service_ = nullptr;
  SyncCredentials::Forget(prefs_);
}

bool CompassSyncService::HasSeed() const {
  return !prefs_->GetString(prefs::kSeed).empty();
}

GURL CompassSyncService::GetServerUrl() const {
  // With --sync-url, the sync service's URL is it.
  return compass::sync::GetServerUrl(
      prefs_,
      sync_service_ ? sync_service_->GetSyncServiceUrlForDebugging() : GURL());
}

bool CompassSyncService::IsChainRunning() const {
  // As compass::sync::IsChainEnabled(), which picks the engine.
  return sync_service_ && sync_service_->IsLocalSyncEnabled() && HasSeed() &&
         GetServerUrl().is_valid() && !left_;
}

// Status ----------------------------------------------------------------------

void CompassSyncService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void CompassSyncService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

SyncStatus CompassSyncService::GetStatus() const {
  SyncStatus status;
  status.server_url = prefs_->GetString(prefs::kServerUrl);
  if (left_) {
    status.state = SyncState::kNeedsRestart;
    status.message = "Restart Compass to finish leaving sync.";
    return status;
  }
  if (!HasSeed()) {
    status.state = SyncState::kOff;
    return status;
  }
  if (!sync_service_) {
    status.state = SyncState::kError;
    status.message = "Sync is turned off for this profile.";
    return status;
  }
  if (!sync_service_->IsLocalSyncEnabled()) {
    status.state = SyncState::kNeedsRestart;
    status.message = "Restart Compass to start syncing.";
    return status;
  }
  if (seed_unreadable_) {
    status.state = SyncState::kError;
    status.message =
        "Couldn't read this device's sync key. Leave sync and join again.";
    return status;
  }
  if (passphrase_rejected_) {
    status.state = SyncState::kError;
    status.message =
        "The data on the server is encrypted for another chain. Leave sync "
        "and join again with the right code.";
    return status;
  }
  if (sync_service_->HasUnrecoverableError()) {
    status.state = SyncState::kError;
    status.message = base::StrCat(
        {"Sync stopped: ",
         sync_service_->GetUnrecoverableErrorMessageForDebugging()});
    return status;
  }
  const syncer::SyncTokenStatus token =
      sync_service_->GetSyncTokenStatusForDebugging();
  if (token.connection_status == syncer::CONNECTION_SERVER_ERROR ||
      token.connection_status == syncer::CONNECTION_AUTH_ERROR) {
    status.state = SyncState::kError;
    status.message =
        base::StrCat({token.connection_status == syncer::CONNECTION_AUTH_ERROR
                          ? "The server refused this device's key"
                          : "Can't reach the server",
                      " (",
                      base::UTF16ToUTF8(base::TimeFormatShortDateAndTime(
                          token.connection_status_update_time)),
                      ")."});
    return status;
  }
  if (!seed_ || types_held_ || setup_handle_ ||
      sync_service_->GetTransportState() !=
          syncer::SyncService::TransportState::ACTIVE) {
    status.state = SyncState::kStarting;
    status.message = "Starting…";
    return status;
  }
  status.state = SyncState::kOn;
  const base::Time last = sync_service_->GetLastSyncedTimeForDebugging();
  status.message =
      last.is_null()
          ? "Syncing…"
          : base::StrCat(
                {"Last synced ",
                 base::UTF16ToUTF8(base::TimeFormatShortDateAndTime(last)),
                 "."});
  return status;
}

PairingStatus CompassSyncService::GetPairingStatus() const {
  return pairing_;
}

void CompassSyncService::NotifyObservers() {
  if (VLOG_IS_ON(1)) {
    // For the end-to-end steps (docs/design/sync-slice-2-steps.md), which
    // run with --vmodule=compass_sync_service=1.
    const SyncStatus status = GetStatus();
    std::string line = base::StrCat(
        {"state=", base::NumberToString(static_cast<int>(status.state)), " ",
         status.message,
         " pairing=", base::NumberToString(static_cast<int>(pairing_.step)),
         " ", pairing_.message});
    if (sync_service_) {
      const std::optional<syncer::PassphraseType> type =
          sync_service_->GetUserSettings()->GetPassphraseType();
      base::StrAppend(
          &line,
          {" local_sync=", sync_service_->IsLocalSyncEnabled() ? "1" : "0",
           " transport=",
           base::NumberToString(
               static_cast<int>(sync_service_->GetTransportState())),
           " passphrase_type=",
           type ? base::NumberToString(static_cast<int>(*type)) : "none",
           " passphrase_required=",
           sync_service_->GetUserSettings()->IsPassphraseRequired() ? "1" : "0",
           " types_held=", types_held_ ? "1" : "0", "\n  active: ",
           syncer::DataTypeSetToDebugString(
               sync_service_->GetActiveDataTypes())});
    }
    if (line != last_logged_) {
      last_logged_ = line;
      VLOG(1) << "[compass-sync] " << line;
    }
  }
  for (Observer& observer : observers_) {
    observer.OnSyncControllerChanged();
  }
}

// Turning sync on and off
// ------------------------------------------------------

void CompassSyncService::StartNewChain(const std::string& server_url,
                                       StartCallback callback) {
  if (HasSeed() || left_) {
    std::move(callback).Run(false, "Sync is already set up. Restart first.",
                            {});
    return;
  }
  const GURL url = ParseServerUrl(server_url);
  if (!url.is_valid()) {
    std::move(callback).Run(
        false, "Enter the server's URL, like https://sync.example.com/v2.", {});
    return;
  }
  const Seed seed = GenerateSeed();
  SaveChain(url, seed,
            base::BindOnce(
                [](StartCallback callback, std::vector<std::string> words,
                   bool ok, std::string error) {
                  std::move(callback).Run(
                      ok, std::move(error),
                      ok ? std::move(words) : std::vector<std::string>());
                },
                std::move(callback), SeedToRecoveryWords(seed)));
}

void CompassSyncService::JoinWithRecoveryCode(const std::string& server_url,
                                              const std::string& words,
                                              DoneCallback callback) {
  if (HasSeed() || left_) {
    std::move(callback).Run(false, "Sync is already set up. Restart first.");
    return;
  }
  const GURL url = ParseServerUrl(server_url);
  if (!url.is_valid()) {
    std::move(callback).Run(
        false, "Enter the server's URL, like https://sync.example.com/v2.");
    return;
  }
  std::optional<Seed> seed = RecoveryCodeToSeed(words);
  if (!seed) {
    std::move(callback).Run(
        false, "That recovery code isn't valid. Check the 12 words.");
    return;
  }
  SaveChain(url, *seed, std::move(callback));
}

void CompassSyncService::SaveChain(const GURL& server_url,
                                   const Seed& seed,
                                   DoneCallback callback) {
  os_crypt_async_->GetInstance(base::BindOnce(
      &CompassSyncService::OnEncryptorForSave, weak_factory_.GetWeakPtr(),
      server_url, seed, std::move(callback)));
}

void CompassSyncService::OnEncryptorForSave(
    const GURL& server_url,
    const Seed& seed,
    DoneCallback callback,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  std::optional<std::vector<uint8_t>> encrypted =
      encryptor
          ? encryptor->EncryptString(std::string(seed.begin(), seed.end()))
          : std::nullopt;
  if (!encrypted) {
    std::move(callback).Run(false, "Couldn't store the sync key.");
    return;
  }
  prefs_->SetString(prefs::kServerUrl, server_url.spec());
  prefs_->SetString(prefs::kSeed, base::Base64Encode(*encrypted));
  // Route 2: the engine runs as local sync, pointed at the server. Chromium
  // reads this when the profile loads.
  prefs_->SetBoolean(syncer::prefs::kEnableLocalSyncBackend, true);
  if (prefs_->GetFilePath(syncer::prefs::kLocalSyncBackendDir).empty()) {
    prefs_->SetFilePath(syncer::prefs::kLocalSyncBackendDir,
                        profile_->GetPath().Append(kLocalSyncDir));
  }
  prefs_->CommitPendingWrite();
  // A chain joined after leaving in the same run still needs the restart.
  std::move(callback).Run(true, std::string());
  NotifyObservers();
}

void CompassSyncService::Leave() {
  CancelPairing();
  prefs_->ClearPref(prefs::kSeed);
  prefs_->SetBoolean(syncer::prefs::kEnableLocalSyncBackend, false);
  prefs_->CommitPendingWrite();
  credentials_->Clear();
  seed_.reset();
  // Keeps the data types from configuring again; requests already fail
  // without the key. The next launch runs the sync service without local
  // sync, which clears sync's metadata (not the data) as signed out.
  if (sync_service_ && sync_service_->IsLocalSyncEnabled() && !setup_handle_) {
    setup_handle_ = sync_service_->GetSetupInProgressHandle();
  }
  left_ = true;
  NotifyObservers();
}

void CompassSyncService::Restart() {
  chrome::AttemptRestart();
}

// The chain at startup
// -----------------------------------------------------------

void CompassSyncService::LoadSeed() {
  os_crypt_async_->GetInstance(base::BindOnce(
      &CompassSyncService::OnEncryptorForLoad, weak_factory_.GetWeakPtr()));
}

void CompassSyncService::OnEncryptorForLoad(
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  std::optional<std::vector<uint8_t>> encrypted =
      base::Base64Decode(prefs_->GetString(prefs::kSeed));
  std::optional<std::string> plain = encryptor && encrypted
                                         ? encryptor->DecryptData(*encrypted)
                                         : std::nullopt;
  if (!plain || plain->size() != kSeedSize) {
    LOG(ERROR) << "Compass sync: can't decrypt the chain's seed";
    seed_unreadable_ = true;
    NotifyObservers();
    return;
  }
  Seed seed;
  std::copy(plain->begin(), plain->end(), seed.begin());
  seed_ = seed;
  credentials_->SetKey(DeriveAuthKey(seed));
  VLOG(1) << "[compass-sync] chain "
          << GetChainId(DeriveAuthKey(seed)).substr(0, 16) << "… on "
          << GetServerUrl().spec();
  UpdatePassphrase();
  NotifyObservers();
}

void CompassSyncService::HoldTypes() {
  types_held_ = true;
  sync_service_->GetUserSettings()->SetSelectedTypes(
      /*sync_everything=*/false, syncer::UserSelectableTypeSet());
}

void CompassSyncService::SelectTypes() {
  syncer::SyncUserSettings* settings = sync_service_->GetUserSettings();
  syncer::UserSelectableTypeSet types =
      settings->GetRegisteredSelectableTypes();
  for (syncer::UserSelectableType type : kUnwantedTypes) {
    types.Remove(type);
  }
  settings->SetSelectedTypes(/*sync_everything=*/false, types);
}

void CompassSyncService::UpdatePassphrase() {
  if (!seed_ || !IsChainRunning() || !sync_service_->IsEngineInitialized()) {
    return;
  }
  syncer::SyncUserSettings* settings = sync_service_->GetUserSettings();
  const std::string passphrase = DerivePassphrase(*seed_);
  if (settings->IsPassphraseRequired()) {
    // A device joining, or one whose Nigori was replaced: the server's data
    // is encrypted with the chain's passphrase. Once per time it's asked.
    if (!decryption_tried_) {
      decryption_tried_ = true;
      passphrase_rejected_ = !settings->SetDecryptionPassphrase(passphrase);
      LOG_IF(ERROR, passphrase_rejected_)
          << "Compass sync: the server's passphrase isn't the chain's";
      VLOG(1) << "[compass-sync] SetDecryptionPassphrase -> "
              << (passphrase_rejected_ ? "REJECTED" : "accepted");
    }
    return;
  }
  decryption_tried_ = false;
  const std::optional<syncer::PassphraseType> type =
      settings->GetPassphraseType();
  if (!type) {
    return;
  }
  if (syncer::IsExplicitPassphrase(*type)) {
    passphrase_rejected_ = false;
    if (types_held_) {
      // Everything is encrypted with the chain's key: let the types start.
      VLOG(1) << "[compass-sync] passphrase in place, data types start";
      types_held_ = false;
      SelectTypes();
    }
    return;
  }
  // The first device of the chain (or a server that lost the data): encrypt
  // everything with the chain's passphrase before anything is committed.
  if (encryption_requested_) {
    return;
  }
  if (!settings->IsCustomPassphraseAllowed()) {
    LOG(ERROR) << "Compass sync: custom passphrase not allowed";
    return;
  }
  encryption_requested_ = true;
  VLOG(1) << "[compass-sync] SetEncryptionPassphrase (was type "
          << static_cast<int>(*type) << ")";
  settings->SetEncryptionPassphrase(passphrase);
}

void CompassSyncService::TriggerRefresh() {
  if (!IsChainRunning() || !seed_ ||
      sync_service_->GetTransportState() !=
          syncer::SyncService::TransportState::ACTIVE) {
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  if (!last_refresh_.is_null() && now - last_refresh_ < kMinRefreshInterval) {
    return;
  }
  last_refresh_ = now;
  sync_service_->TriggerRefresh(
      syncer::SyncService::TriggerRefreshSource::kUnknown,
      sync_service_->GetActiveDataTypes());
}

void CompassSyncService::OnStateChanged(syncer::SyncService* sync) {
  UpdatePassphrase();
  NotifyObservers();
}

void CompassSyncService::OnSyncCycleCompleted(syncer::SyncService* sync) {
  NotifyObservers();
}

void CompassSyncService::OnSyncShutdown(syncer::SyncService* sync) {
  setup_handle_.reset();
  sync_observation_.Reset();
  sync_service_ = nullptr;
}

void CompassSyncService::OnBrowserActivated(BrowserWindowInterface* browser) {
  TriggerRefresh();
}

void CompassSyncService::OnResume() {
  TriggerRefresh();
}

// Pairing --------------------------------------------------------------------

void CompassSyncService::SetPairing(PairingStatus status) {
  pairing_ = std::move(status);
  NotifyObservers();
}

void CompassSyncService::AddDevice() {
  if (host_ || joiner_ || !seed_ || !IsChainRunning()) {
    return;
  }
  const GURL server_url = GetServerUrl();
  if (!server_url.is_valid()) {
    SetPairing({PairingStep::kFailed, {}, {}, "No server is set."});
    return;
  }
  auto transport = std::make_unique<NetworkPairingTransport>(
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess(),
      server_url,
      base::BindRepeating(
          [](scoped_refptr<SyncCredentials> credentials) {
            return credentials->MakeToken(base::Time::Now())
                .value_or(std::string());
          },
          credentials_));
  host_ = std::make_unique<PairingHost>(
      std::move(transport), std::string(server_url.host()), *seed_, this);
  SetPairing({PairingStep::kWorking, {}, {}, {}});
  host_->Start();
}

void CompassSyncService::OnPairingCode(const std::string& code,
                                       base::TimeDelta expires_in) {
  SetPairing({PairingStep::kShowingCode,
              code,
              {},
              "Enter this code on the other device within 5 minutes."});
}

void CompassSyncService::OnPairingRequest(const std::string& device_name) {
  SetPairing({PairingStep::kConfirm, pairing_.code, device_name, {}});
}

void CompassSyncService::AnswerPairing(bool allow) {
  if (host_ && pairing_.step == PairingStep::kConfirm) {
    SetPairing({PairingStep::kWorking, {}, {}, {}});
    host_->Answer(allow);
  }
}

void CompassSyncService::OnPairingFinished(PairingHost::Result result) {
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                             std::move(host_));
  SetPairing({result == PairingHost::Result::kAdded ||
                      result == PairingHost::Result::kDenied
                  ? PairingStep::kDone
                  : PairingStep::kFailed,
              {},
              {},
              HostResultMessage(result)});
}

void CompassSyncService::JoinWithPairingCode(const std::string& server_url,
                                             const std::string& code) {
  if (host_ || joiner_ || HasSeed() || left_) {
    return;
  }
  const GURL url = ParseServerUrl(server_url);
  if (!url.is_valid()) {
    SetPairing({PairingStep::kFailed,
                {},
                {},
                "Enter the server's URL, like https://sync.example.com/v2."});
    return;
  }
  SetPairing({PairingStep::kWorking, {}, {}, {}});
  // The name A shows ("Add MacBook?") is the computer's name.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&syncer::GetPersonalizableDeviceNameBlocking),
      base::BindOnce(&CompassSyncService::StartJoin, weak_factory_.GetWeakPtr(),
                     url, code));
}

void CompassSyncService::StartJoin(const GURL& server_url,
                                   const std::string& code,
                                   const std::string& device_name) {
  if (pairing_.step != PairingStep::kWorking || joiner_) {
    return;  // Canceled meanwhile.
  }
  joiner_ = std::make_unique<PairingJoiner>(
      std::make_unique<NetworkPairingTransport>(
          profile_->GetDefaultStoragePartition()
              ->GetURLLoaderFactoryForBrowserProcess(),
          server_url, NetworkPairingTransport::TokenGetter()),
      std::string(server_url.host()), device_name);
  joiner_->Start(code,
                 base::BindOnce(&CompassSyncService::SetPairing,
                                weak_factory_.GetWeakPtr(),
                                PairingStatus{PairingStep::kWaitingForApproval,
                                              {},
                                              {},
                                              "Confirm on the other device."}),
                 base::BindOnce(&CompassSyncService::OnJoinFinished,
                                weak_factory_.GetWeakPtr(), server_url));
}

void CompassSyncService::OnJoinFinished(const GURL& server_url,
                                        PairingJoiner::Result result,
                                        std::optional<Seed> seed) {
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
      FROM_HERE, std::move(joiner_));
  if (result != PairingJoiner::Result::kJoined || !seed) {
    SetPairing({PairingStep::kFailed, {}, {}, JoinResultMessage(result)});
    return;
  }
  SaveChain(server_url, *seed,
            base::BindOnce(
                [](base::WeakPtr<CompassSyncService> self, bool ok,
                   std::string error) {
                  if (self) {
                    self->SetPairing(
                        {ok ? PairingStep::kDone : PairingStep::kFailed,
                         {},
                         {},
                         ok ? JoinResultMessage(PairingJoiner::Result::kJoined)
                            : error});
                  }
                },
                weak_factory_.GetWeakPtr()));
}

void CompassSyncService::CancelPairing() {
  host_.reset();
  joiner_.reset();
  SetPairing(PairingStatus());
}

GURL ParseServerUrl(std::string_view text) {
  GURL url(base::TrimWhitespaceASCII(text, base::TRIM_ALL));
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || !url.has_host()) {
    return GURL();
  }
  return url;
}

}  // namespace compass::sync
