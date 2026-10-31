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
#include "chrome/browser/sync/device_info_sync_service_factory.h"
#include "compass/browser/sync/sync_settings.h"
#include "compass/components/sync/compass_sync_notifier.h"
#include "compass/components/sync/pairing_network_transport.h"
#include "compass/components/sync/pref_names.h"
#include "compass/components/sync/sync_credentials.h"
#include "compass/components/sync/sync_hooks.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/signin/public/base/gaia_id_hash.h"
#include "components/sync/base/data_type.h"
#include "components/sync/base/passphrase_enums.h"
#include "components/sync/base/pref_names.h"
#include "components/sync/base/user_selectable_type.h"
#include "components/sync/engine/cycle/model_neutral_state.h"
#include "components/sync/engine/cycle/sync_cycle_snapshot.h"
#include "components/sync/engine/sync_status.h"
#include "components/sync/service/glue/sync_transport_data_prefs.h"
#include "components/sync/service/sync_service.h"
#include "components/sync/service/sync_token_status.h"
#include "components/sync/service/sync_user_settings.h"
#include "components/sync_device_info/device_info.h"
#include "components/sync_device_info/device_info_sync_service.h"
#include "components/sync_device_info/local_device_info_provider.h"
#include "components/sync_device_info/local_device_info_util.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/storage_partition.h"
#include "google_apis/gaia/gaia_id.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace compass::sync {
namespace {

// Where local sync would keep its file. Never written (the engine talks to
// the server instead), but local sync doesn't start on macOS without it.
constexpr base::FilePath::CharType kLocalSyncDir[] =
    FILE_PATH_LITERAL("Compass Sync");

// Focus and wake ask the server for changes at most this often.
constexpr base::TimeDelta kMinRefreshInterval = base::Seconds(10);

// How often the server's quota status is asked for (slice 7): normally, and
// while commits fail or a limit is in the way.
constexpr base::TimeDelta kQuotaCheckInterval = base::Minutes(30);
constexpr base::TimeDelta kQuotaRecheckInterval = base::Minutes(1);

// The types Compass never syncs are GetUnwantedSelectableTypes()
// (sync_settings.h). History (kHistory: HISTORY and
// HISTORY_DELETE_DIRECTIVES) is wanted and selected like the rest; it runs
// under local sync, encrypted, since slice 2b (sync.md, "Slice 2b notes").
// Its USER_EVENTS and JOURNEY stay off: local sync leaves them out.

std::string HostResultMessage(PairingHost::Result result) {
  switch (result) {
    case PairingHost::Result::kAdded:
      return "Device added. It starts syncing after a restart.";
    case PairingHost::Result::kDenied:
      return "Not added.";
    case PairingHost::Result::kWrongCode:
      return "Wrong code. Try again with a new code.";
    case PairingHost::Result::kCanceled:
      return "The other device canceled.";
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
      return "Not added: the other device said no.";
    case PairingJoiner::Result::kBadCode:
      return "Enter the code as shown, like 7-482913.";
    case PairingJoiner::Result::kNoSuchCode:
      return "No such code on this server, or it expired.";
    case PairingJoiner::Result::kCodeUsed:
      return "That code was already used. Ask for a new one.";
    case PairingJoiner::Result::kWrongCode:
      return "Wrong code. Ask the other device for a new one.";
    case PairingJoiner::Result::kCanceled:
      return "The other device canceled. Ask for a new code.";
    case PairingJoiner::Result::kClosed:
      return "The other device ended pairing. Check the code and ask for a "
             "new one.";
    case PairingJoiner::Result::kExpired:
      return "Code expired. Ask for a new one.";
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
      device_info_(sync_service
                       ? DeviceInfoSyncServiceFactory::GetForProfile(profile)
                       : nullptr),
      os_crypt_async_(os_crypt_async),
      credentials_(SyncCredentials::ForPrefs(profile->GetPrefs())) {
  if (sync_service_) {
    sync_observation_.Observe(sync_service_);
  }
  if (device_info_ && device_info_->GetDeviceInfoTracker()) {
    device_info_observation_.Observe(device_info_->GetDeviceInfoTracker());
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
  registry->RegisterBooleanPref(prefs::kResetPending, false);
  RegisterSyncSettingsPrefs(registry);
}

void CompassSyncService::Shutdown() {
  notifier_.reset();
  quota_fetcher_.reset();
  host_.reset();
  joiner_.reset();
  setup_handle_.reset();
  sync_observation_.Reset();
  device_info_observation_.Reset();
  device_info_ = nullptr;
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

bool CompassSyncService::AreDataTypesReleased() const {
  return AreTypesSelected() &&
         !GetDisabledSyncDataTypes(prefs_).contains(SyncDataType::kSpaces);
}

bool CompassSyncService::AreTypesSelected() const {
  return IsChainRunning() && seed_ && !types_held_ && !setup_handle_;
}

bool CompassSyncService::IsChainRunning() const {
  // As compass::sync::IsChainEnabled(), which picks the engine.
  return sync_service_ && sync_service_->IsLocalSyncEnabled() && HasSeed() &&
         GetServerUrl().is_valid() && !left_ && !reset_;
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
  if (reset_) {
    status.state = SyncState::kNeedsRestart;
    status.message =
        "Restart Compass to start the new sync chain. Then add your other "
        "devices again.";
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
  // Slice 7: a server limit in the way turns this into its message.
  ApplySyncLimit(GetLimit(), status);
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
  prefs_->ClearPref(prefs::kResetPending);
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
  UpdateNotifier();
  if (sync_service_) {
    sync_service_->DataTypePreconditionChanged(syncer::COMPASS_SPACES);
  }
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
  HoldSelectableTypes(sync_service_->GetUserSettings());
  // COMPASS_SPACES isn't selectable: its controller asks
  // AreDataTypesReleased().
  sync_service_->DataTypePreconditionChanged(syncer::COMPASS_SPACES);
}

void CompassSyncService::SelectTypes() {
  SelectSelectableTypes(sync_service_->GetUserSettings(),
                        GetDisabledSyncDataTypes(prefs_));
  sync_service_->DataTypePreconditionChanged(syncer::COMPASS_SPACES);
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
      passphrase_set_here_ = !passphrase_rejected_;
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
    if (types_held_ && prefs_->GetBoolean(prefs::kResetPending) &&
        !passphrase_set_here_) {
      // After "Reset sync": the explicit passphrase seen first is the old
      // chain's, cached, until the engine (fresh, see OnChainReset())
      // learns the new chain has none and this device sets it.
      return;
    }
    if (types_held_) {
      prefs_->ClearPref(prefs::kResetPending);
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
  passphrase_set_here_ = true;
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

void CompassSyncService::UpdateNotifier() {
  if (!sync_service_ || !IsChainRunning() || !seed_) {
    notifier_.reset();
    return;
  }
  if (notifier_ || !AreDataTypesReleased() ||
      sync_service_->GetTransportState() !=
          syncer::SyncService::TransportState::ACTIVE) {
    return;
  }
  notifier_ = std::make_unique<CompassSyncNotifier>(
      GetServerUrl(),
      MakeNetworkEventStreamFactory(
          profile_->GetDefaultStoragePartition()
              ->GetURLLoaderFactoryForBrowserProcess()),
      base::BindRepeating(
          [](scoped_refptr<SyncCredentials> credentials) {
            return credentials->MakeToken(base::Time::Now());
          },
          credentials_),
      // The device's cache GUID (its DEVICE_INFO entry's), so the server
      // leaves out this device's own commits.
      base::BindRepeating(
          [](Profile* profile) -> std::string {
            syncer::DeviceInfoSyncService* device_info =
                DeviceInfoSyncServiceFactory::GetForProfile(profile);
            const syncer::DeviceInfo* local =
                device_info ? device_info->GetLocalDeviceInfoProvider()
                                  ->GetLocalDeviceInfo()
                            : nullptr;
            return local ? local->guid() : std::string();
          },
          base::Unretained(profile_.get())),
      // Not TriggerRefresh() above: that one waits kMinRefreshInterval
      // between focus and wake refreshes; a notification is a real change
      // elsewhere and the notifier already merges bursts.
      base::BindRepeating(
          [](base::WeakPtr<CompassSyncService> self,
             syncer::DataTypeSet types) {
            if (!self || !self->sync_service_ ||
                self->sync_service_->GetTransportState() !=
                    syncer::SyncService::TransportState::ACTIVE) {
              return;
            }
            types.RetainAll(self->sync_service_->GetActiveDataTypes());
            if (!types.empty()) {
              self->sync_service_->TriggerRefresh(
                  syncer::SyncService::TriggerRefreshSource::
                      kSyncInvalidationsService,
                  types);
            }
          },
          weak_factory_.GetWeakPtr()),
      content::GetNetworkConnectionTracker());
  notifier_->Start();
}

// Quota (slice 7) ---------------------------------------------------------------

void CompassSyncService::MaybeCheckQuota() {
  if (!sync_service_ || !IsChainRunning() || !seed_ ||
      sync_service_->GetTransportState() !=
          syncer::SyncService::TransportState::ACTIVE) {
    return;
  }
  // The engine retries a commit refused for quota as a transient error;
  // that's when to look soon.
  const bool commit_failed =
      sync_service_->GetLastCycleSnapshotForDebugging()
          .model_neutral_state()
          .commit_result.type() != syncer::SyncerError::Type::kSuccess;
  const bool limited =
      quota_ && (quota_->IsServerFull() || quota_->history_full);
  const base::TimeTicks now = base::TimeTicks::Now();
  if (!last_quota_check_.is_null() &&
      now - last_quota_check_ < (commit_failed || limited
                                     ? kQuotaRecheckInterval
                                     : kQuotaCheckInterval)) {
    return;
  }
  if (!quota_fetcher_) {
    quota_fetcher_ = std::make_unique<QuotaFetcher>(
        profile_->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess(),
        GetServerUrl(),
        base::BindRepeating(
            [](scoped_refptr<SyncCredentials> credentials) {
              return credentials->MakeToken(base::Time::Now());
            },
            credentials_));
  }
  if (quota_fetcher_->Fetch(base::BindOnce(&CompassSyncService::OnQuotaStatus,
                                           weak_factory_.GetWeakPtr()))) {
    last_quota_check_ = now;
  }
}

void CompassSyncService::OnQuotaStatus(std::optional<QuotaStatus> status) {
  // No answer (an older server, the network): keep what was known.
  if (!status) {
    return;
  }
  VLOG(1) << "[compass-sync] quota: entities " << status->entities << "/"
          << status->max_entities << ", bytes " << status->bytes << "/"
          << status->max_bytes << ", history " << status->history << "/"
          << status->max_history << " (" << status->history_days
          << " days), full:" << (status->entities_full ? " entities" : "")
          << (status->bytes_full ? " bytes" : "")
          << (status->history_full ? " history" : "");
  quota_ = std::move(status);
  NotifyObservers();
}

SyncLimit CompassSyncService::GetLimit() const {
  bool throttled = false;
  if (sync_service_) {
    syncer::SyncStatus engine_status;
    if (sync_service_->QueryDetailedSyncStatusForDebugging(&engine_status)) {
      throttled = !engine_status.throttled_types.empty();
    }
  }
  return GetSyncLimit(throttled, quota_ && quota_->IsServerFull(),
                      quota_ && quota_->history_full);
}

void CompassSyncService::OnStateChanged(syncer::SyncService* sync) {
  UpdatePassphrase();
  UpdateNotifier();
  NotifyObservers();
}

void CompassSyncService::OnSyncCycleCompleted(syncer::SyncService* sync) {
  MaybeCheckQuota();
  NotifyObservers();
}

void CompassSyncService::OnDeviceInfoChange() {
  NotifyObservers();
}

void CompassSyncService::OnSyncShutdown(syncer::SyncService* sync) {
  notifier_.reset();
  quota_fetcher_.reset();
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

// The page's details ------------------------------------------------------

std::vector<SyncDevice> CompassSyncService::GetDevices() const {
  std::vector<SyncDevice> devices;
  if (!IsChainRunning() || !device_info_ ||
      !device_info_->GetDeviceInfoTracker()) {
    return devices;
  }
  const syncer::DeviceInfoTracker* tracker =
      device_info_->GetDeviceInfoTracker();
  const syncer::DeviceInfo* local =
      device_info_->GetLocalDeviceInfoProvider()
          ? device_info_->GetLocalDeviceInfoProvider()->GetLocalDeviceInfo()
          : nullptr;
  for (const syncer::DeviceInfo* info : tracker->GetAllDeviceInfo()) {
    const bool this_device = local && local->guid() == info->guid();
    if (!this_device && tracker->IsRecentLocalCacheGuid(info->guid())) {
      // This device in an earlier sync setup (before leaving and joining
      // again); it expires on the server.
      continue;
    }
    SyncDevice device;
    device.name = info->client_name();
    device.kind = GetDeviceKind(info->os_type(), info->form_factor());
    device.last_seen = info->last_updated_timestamp();
    device.this_device = this_device;
    devices.push_back(std::move(device));
  }
  // This device first, then the most recently seen.
  std::ranges::sort(devices, [](const SyncDevice& a, const SyncDevice& b) {
    if (a.this_device != b.this_device) {
      return a.this_device;
    }
    return a.last_seen > b.last_seen;
  });
  return devices;
}

std::vector<SyncDataTypeState> CompassSyncService::GetDataTypes() const {
  const SyncDataTypeSet disabled = GetDisabledSyncDataTypes(prefs_);
  std::vector<SyncDataTypeState> types;
  for (SyncDataType type : kAllSyncDataTypes) {
    types.push_back({type, !disabled.contains(type)});
  }
  return types;
}

void CompassSyncService::SetDataTypeEnabled(SyncDataType type, bool enabled) {
  SetSyncDataTypeEnabled(prefs_, type, enabled);
  // While the types are held (no passphrase yet), the choice only waits in
  // the pref: SelectTypes() reads it when they're released.
  if (sync_service_ && IsChainRunning()) {
    if (type == SyncDataType::kSpaces) {
      sync_service_->DataTypePreconditionChanged(syncer::COMPASS_SPACES);
    } else if (AreTypesSelected()) {
      SelectTypes();
    }
  }
  NotifyObservers();
}

std::vector<std::string> CompassSyncService::GetRecoveryCode() const {
  if (!seed_ || left_ || reset_) {
    return {};
  }
  return SeedToRecoveryWords(*seed_);
}

void CompassSyncService::ResetChain(StartCallback callback) {
  if (!seed_ || !IsChainRunning()) {
    std::move(callback).Run(false, "Sync isn't running on this device.", {});
    return;
  }
  // The pref's URL, or --sync-url's.
  GURL server_url = ParseServerUrl(prefs_->GetString(prefs::kServerUrl));
  if (!server_url.is_valid()) {
    server_url = GetServerUrl();
  }
  CancelPairing();
  // This run's engine stops talking to the old chain (its requests fail
  // without a key) and configuring; the new chain starts after a restart.
  credentials_->Clear();
  if (!setup_handle_) {
    setup_handle_ = sync_service_->GetSetupInProgressHandle();
  }
  reset_ = true;
  seed_.reset();
  sync_service_->DataTypePreconditionChanged(syncer::COMPASS_SPACES);
  const Seed seed = GenerateSeed();
  // SaveChain() replaces the seed; the server URL stays.
  SaveChain(server_url, seed,
            base::BindOnce(&CompassSyncService::OnChainReset,
                           weak_factory_.GetWeakPtr(), std::move(callback),
                           SeedToRecoveryWords(seed)));
}

void CompassSyncService::OnChainReset(StartCallback callback,
                                      std::vector<std::string> words,
                                      bool ok,
                                      std::string error) {
  if (!ok) {
    // The old seed is still in the pref: the old chain resumes at the next
    // launch.
    std::move(callback).Run(false, std::move(error), {});
    return;
  }
  // Forget this device's place in the old chain: at the next launch the
  // engine finds no cache GUID, makes a new one and starts afresh (every
  // type's metadata, Nigori's included, is dropped on the GUID mismatch),
  // so this device's data goes up to the new chain as a first sync. Local
  // sync has no account, so its transport data is keyed to the empty Gaia
  // ID.
  syncer::SyncTransportDataPrefs(prefs_,
                                 signin::GaiaIdHash::FromGaiaId(GaiaId()))
      .ClearForCurrentAccount();
  prefs_->SetBoolean(prefs::kResetPending, true);
  prefs_->CommitPendingWrite();
  VLOG(1) << "[compass-sync] chain reset; restart to start the new one";
  std::move(callback).Run(true, std::string(), std::move(words));
  NotifyObservers();
}

GURL ParseServerUrl(std::string_view text) {
  GURL url(base::TrimWhitespaceASCII(text, base::TRIM_ALL));
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || !url.has_host()) {
    return GURL();
  }
  return url;
}

}  // namespace compass::sync
