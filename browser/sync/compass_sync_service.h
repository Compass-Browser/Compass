// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_COMPASS_SYNC_SERVICE_H_
#define COMPASS_BROWSER_SYNC_COMPASS_SYNC_SERVICE_H_

#include <memory>
#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/power_monitor/power_observer.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "compass/browser/sync/sync_controller.h"
#include "compass/components/sync/chain.h"
#include "compass/components/sync/pairing.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/sync/service/sync_service_observer.h"
#include "components/sync_device_info/device_info_tracker.h"
#include "url/gurl.h"

class PrefService;
class Profile;

namespace os_crypt_async {
class Encryptor;
class OSCryptAsync;
}  // namespace os_crypt_async

namespace syncer {
class DeviceInfoSyncService;
class SyncService;
class SyncSetupInProgressHandle;
}  // namespace syncer

namespace user_prefs {
class PrefRegistrySyncable;
}  // namespace user_prefs

namespace compass::sync {

class CompassSyncNotifier;
class SyncCredentials;

// A profile's place in a sync chain (docs/design/sync.md, slice 2): keeps
// the seed (encrypted in a pref), hands the chain key to the engine, sets
// the custom passphrase, picks the data types, refreshes on focus and wake,
// and runs pairing on either side.
//
// Chromium's sync service decides at profile load whether it runs local
// sync, which is how Compass runs it (route 2), so turning sync on or off
// takes effect at the next launch.
class CompassSyncService : public KeyedService,
                           public SyncController,
                           public syncer::SyncServiceObserver,
                           public syncer::DeviceInfoTracker::Observer,
                           public BrowserCollectionObserver,
                           public base::PowerSuspendObserver,
                           public PairingHost::Delegate {
 public:
  // SyncController's, not DeviceInfoTracker's (both bases have one).
  using Observer = SyncController::Observer;

  // |sync_service| is null if sync is disabled for the profile.
  CompassSyncService(Profile* profile,
                     syncer::SyncService* sync_service,
                     os_crypt_async::OSCryptAsync* os_crypt_async);
  CompassSyncService(const CompassSyncService&) = delete;
  CompassSyncService& operator=(const CompassSyncService&) = delete;
  ~CompassSyncService() override;

  static void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);

  // Whether COMPASS_SPACES may run (compass_spaces_sync.cc asks): the chain
  // is running, its passphrase is in place (when the selectable types are
  // selected), and the page's Spaces switch is on.
  bool AreDataTypesReleased() const;

  // KeyedService:
  void Shutdown() override;

  // SyncController:
  void AddObserver(Observer* observer) override;
  void RemoveObserver(Observer* observer) override;
  SyncStatus GetStatus() const override;
  PairingStatus GetPairingStatus() const override;
  void StartNewChain(const std::string& server_url,
                     StartCallback callback) override;
  void JoinWithRecoveryCode(const std::string& server_url,
                            const std::string& words,
                            DoneCallback callback) override;
  void JoinWithPairingCode(const std::string& server_url,
                           const std::string& code) override;
  void AddDevice() override;
  void AnswerPairing(bool allow) override;
  void CancelPairing() override;
  void Leave() override;
  void Restart() override;
  std::vector<SyncDevice> GetDevices() const override;
  std::vector<SyncDataTypeState> GetDataTypes() const override;
  void SetDataTypeEnabled(SyncDataType type, bool enabled) override;
  std::vector<std::string> GetRecoveryCode() const override;
  void ResetChain(StartCallback callback) override;

  // syncer::DeviceInfoTracker::Observer:
  void OnDeviceInfoChange() override;

  // syncer::SyncServiceObserver:
  void OnStateChanged(syncer::SyncService* sync) override;
  void OnSyncCycleCompleted(syncer::SyncService* sync) override;
  void OnSyncShutdown(syncer::SyncService* sync) override;

  // BrowserCollectionObserver:
  void OnBrowserActivated(BrowserWindowInterface* browser) override;

  // base::PowerSuspendObserver:
  void OnResume() override;

  // PairingHost::Delegate:
  void OnPairingCode(const std::string& code,
                     base::TimeDelta expires_in) override;
  void OnPairingRequest(const std::string& device_name) override;
  void OnPairingFinished(PairingHost::Result result) override;

 private:
  // Whether this run's sync engine talks to the chain's server.
  bool IsChainRunning() const;
  // The passphrase is in place: the selectable types are selected.
  bool AreTypesSelected() const;
  bool HasSeed() const;
  GURL GetServerUrl() const;

  void LoadSeed();
  void OnEncryptorForLoad(scoped_refptr<os_crypt_async::Encryptor> encryptor);
  void SaveChain(const GURL& server_url,
                 const Seed& seed,
                 DoneCallback callback);
  void OnEncryptorForSave(const GURL& server_url,
                          const Seed& seed,
                          DoneCallback callback,
                          scoped_refptr<os_crypt_async::Encryptor> encryptor);

  // Selects no type, until the passphrase is in place.
  void HoldTypes();
  // Selects the types of the page's switches, except those Compass never
  // syncs.
  void SelectTypes();
  // After ResetChain() saved the new seed.
  void OnChainReset(StartCallback callback,
                    std::vector<std::string> words,
                    bool ok,
                    std::string error);
  // Sets or supplies the chain's passphrase, and lets the data types start
  // once it's in place.
  void UpdatePassphrase();
  void TriggerRefresh();
  // Runs the change notifier while the chain's data types run, stops it
  // when the chain stops (slice 6).
  void UpdateNotifier();

  void StartJoin(const GURL& server_url,
                 const std::string& code,
                 const std::string& device_name);
  void OnJoinFinished(const GURL& server_url,
                      PairingJoiner::Result result,
                      std::optional<Seed> seed);
  void SetPairing(PairingStatus status);
  void NotifyObservers();

  const raw_ptr<Profile> profile_;
  const raw_ptr<PrefService> prefs_;
  raw_ptr<syncer::SyncService> sync_service_;
  raw_ptr<syncer::DeviceInfoSyncService> device_info_;
  const raw_ptr<os_crypt_async::OSCryptAsync> os_crypt_async_;
  const scoped_refptr<SyncCredentials> credentials_;

  // The decrypted seed, once loaded.
  std::optional<Seed> seed_;
  bool seed_unreadable_ = false;
  // True while no data type is selected, until the passphrase is in place,
  // so nothing is committed with a key the server knows.
  bool types_held_ = false;
  // Only after Leave(): keeps the data types from configuring again.
  std::unique_ptr<syncer::SyncSetupInProgressHandle> setup_handle_;
  bool decryption_tried_ = false;
  bool passphrase_rejected_ = false;
  bool encryption_requested_ = false;
  // This run set the passphrase, or had it accepted: after a reset, only
  // then do the types start (prefs::kResetPending).
  bool passphrase_set_here_ = false;
  // Left the chain (or joined one) during this run.
  bool left_ = false;
  // Replaced the chain during this run; the new one starts after a restart.
  bool reset_ = false;
  base::TimeTicks last_refresh_;
  std::string last_logged_;

  std::unique_ptr<PairingHost> host_;
  std::unique_ptr<PairingJoiner> joiner_;
  std::unique_ptr<CompassSyncNotifier> notifier_;
  PairingStatus pairing_;

  base::ObserverList<Observer> observers_;
  base::ScopedObservation<syncer::SyncService, syncer::SyncServiceObserver>
      sync_observation_{this};
  base::ScopedObservation<syncer::DeviceInfoTracker,
                          syncer::DeviceInfoTracker::Observer>
      device_info_observation_{this};
  base::ScopedObservation<ProfileBrowserCollection, BrowserCollectionObserver>
      browser_observation_{this};
  bool observing_power_ = false;

  base::WeakPtrFactory<CompassSyncService> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_BROWSER_SYNC_COMPASS_SYNC_SERVICE_H_
