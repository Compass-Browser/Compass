// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_COMPASS_SYNC_SERVICE_H_
#define COMPASS_BROWSER_SYNC_COMPASS_SYNC_SERVICE_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/power_monitor/power_observer.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_attributes_storage_observer.h"
#if BUILDFLAG(IS_ANDROID)
#include "base/android/application_status_listener.h"
#else
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#endif
#include "compass/browser/sync/sync_controller.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/components/sync/chain.h"
#include "compass/components/sync/compass_sync_quota.h"
#include "compass/components/sync/pairing.h"
#include "compass/components/sync/profile_list.h"
#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"
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

class CompassProfileCreator;
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
//
// Profiles (docs/design/sync.md, "Profiles"): every profile
// the switcher lists is in the chain, each with its own account. The one
// that started or joined the chain is the home profile: its account is the
// chain's own, and its service also keeps the chain's profile list (through
// its COMPASS_SPACES bridge), creates the chain's profiles this device lacks,
// puts this device's new profiles in the chain, and runs the mapping step of
// a join. Leaving leaves with every profile.
class CompassSyncService : public KeyedService,
                           public SyncController,
                           public syncer::SyncServiceObserver,
                           public syncer::DeviceInfoTracker::Observer,
#if !BUILDFLAG(IS_ANDROID)
                           public BrowserCollectionObserver,
#endif
                           public base::PowerSuspendObserver,
                           public PairingHost::Delegate,
                           public ProfileAttributesStorageObserver {
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

  // Profiles (slice 5).
  //
  // The profile's sync UUID (compass.sync.profile_uuid).
  std::string GetProfileUuid() const;
  // Whether the profile is the chain's home here: it has the seed and syncs
  // with the chain's own account.
  bool IsHome() const;
  // For the home profile's COMPASS_SPACES bridge: the chain's profiles this
  // device has, the home first. Nullopt unless this is the home profile and
  // its chain runs.
  std::optional<std::vector<sync_pb::CompassProfile>> GetLocalChainProfiles()
      const;
  // The home profile's bridge has the chain's list: |profiles|.
  void OnChainProfiles(const std::vector<sync_pb::CompassProfile>& profiles);

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
  ProfileMappingStatus GetProfileMapping() const override;
  void SetProfileMapping(const ProfileMapping& mapping,
                         DoneCallback callback) override;

  // syncer::DeviceInfoTracker::Observer:
  void OnDeviceInfoChange() override;

  // syncer::SyncServiceObserver:
  void OnStateChanged(syncer::SyncService* sync) override;
  void OnSyncCycleCompleted(syncer::SyncService* sync) override;
  void OnSyncShutdown(syncer::SyncService* sync) override;

#if !BUILDFLAG(IS_ANDROID)
  // BrowserCollectionObserver:
  void OnBrowserActivated(BrowserWindowInterface* browser) override;
#endif

  // base::PowerSuspendObserver:
  void OnResume() override;

  // PairingHost::Delegate:
  void OnPairingCode(const std::string& code,
                     base::TimeDelta expires_in) override;
  void OnPairingRequest(const std::string& device_name) override;
  void OnPairingFinished(PairingHost::Result result) override;

  // ProfileAttributesStorageObserver:
  void OnProfileNameChanged(const base::FilePath& profile_path,
                            const std::u16string& old_profile_name) override;
  void OnProfileAvatarChanged(const base::FilePath& profile_path) override;
  void OnProfileWillBeRemoved(const base::FilePath& profile_path) override;

 private:
  // SpaceDirectory's observer (a member: SyncController and SpaceDirectory
  // both call theirs Observer).
  class DirectoryObserver : public SpaceDirectory::Observer {
   public:
    explicit DirectoryObserver(CompassSyncService* service);
    DirectoryObserver(const DirectoryObserver&) = delete;
    DirectoryObserver& operator=(const DirectoryObserver&) = delete;
    ~DirectoryObserver() override;

    // SpaceDirectory::Observer:
    void OnDirectoryChanged() override;

   private:
    const raw_ptr<CompassSyncService> service_;
  };

  // A join waiting for the user to map this device's other profiles to the
  // chain's (pairing brought the chain's list).
  struct PendingJoin {
    PendingJoin();
    PendingJoin(PendingJoin&&);
    PendingJoin& operator=(PendingJoin&&);
    ~PendingJoin();

    GURL server_url;
    Seed seed{};
    std::vector<ChainProfile> chain;
  };

  // Whether this run's sync engine talks to the chain's server.
  bool IsChainRunning() const;
  // The passphrase is in place: the selectable types are selected.
  bool AreTypesSelected() const;
  bool HasSeed() const;
  GURL GetServerUrl() const;

  // Slice 5.
  //
  // Whether the profile syncs with its own account (not the home).
  bool IsProfileAccount() const;
  // The UUID in the account's derivations: empty for the home profile.
  std::string AccountUuid() const;
  // This device's other profiles that can sync: those the switcher lists.
  std::vector<LocalProfile> GetOtherLocalProfiles() const;
  // The home profile's service on this device, if any.
  static CompassSyncService* FindHome();
  // The list pairing hands to a new device.
  std::vector<ChainProfile> GetChainProfilesForPairing() const;
  // This profile becomes the chain's home here (with |chain|'s home UUID),
  // saves the chain, then puts the other profiles in it as |mapping| says.
  // |profiles_pending|: the others wait for the chain's list instead.
  void FinishJoin(const GURL& server_url,
                  const Seed& seed,
                  const std::vector<ChainProfile>& chain,
                  ProfileMapping mapping,
                  bool profiles_pending,
                  DoneCallback callback);
  // Puts this device's other profiles in the chain as |mapping| says. Home
  // profile, chain saved.
  void ApplyMapping(const ProfileMapping& mapping);
  // Puts |profile| in the chain (its own account, |uuid|), from the next
  // launch on. Home profile, chain saved.
  void EnrollProfile(Profile* profile, const std::string& uuid);
  // Home profile: names and avatars as the chain has them.
  void ApplyChainNames();
  // Home profile: this device's listed profiles join the chain; the chain's
  // profiles this device doesn't have are created.
  void ReconcileProfiles();
  // Home profile, once per run before ReconcileProfiles() creates any
  // profile: reads every profile directory's sync prefs (those not loaded
  // from disk), records which directory is each profile of the chain, and
  // retires the copies earlier runs made (PlanProfileDirs()).
  void ScanProfileDirs();
  void OnProfileDirsScanned(std::vector<ProfileDir> dirs);
  // Leaves the chain with this profile only.
  void LeaveProfile();
  // The switcher changed: a profile may have been listed.
  void OnSpaceDirectoryChanged();

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
  // ResetChain(), for each profile of the chain here: stops this run's
  // engine; the new chain starts after a restart.
  void StopForReset();
  // After a reset: clears this profile's sync transport data, so the next
  // launch starts afresh, and holds the types until the new chain's
  // passphrase is in place (prefs::kResetPending).
  void ForgetOldChain();
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

  // Slice 5.
  std::optional<PendingJoin> pending_join_;
  // Home profile: the chain's profiles as its bridge last had them.
  std::vector<ChainProfile> chain_profiles_;
  bool chain_profiles_known_ = false;
#if !BUILDFLAG(IS_ANDROID)
  // Android has one profile: the chain's others aren't created there.
  std::unique_ptr<CompassProfileCreator> profile_creator_;
#endif
  std::unique_ptr<MissingProfiles> missing_profiles_;
  bool scanning_dirs_ = false;
  bool dirs_scanned_ = false;
  DirectoryObserver directory_observer_{this};
  bool observing_directory_ = false;
  base::ScopedObservation<ProfileAttributesStorage,
                          ProfileAttributesStorageObserver>
      attributes_observation_{this};

  base::ObserverList<Observer> observers_;
  base::ScopedObservation<syncer::SyncService, syncer::SyncServiceObserver>
      sync_observation_{this};
  base::ScopedObservation<syncer::DeviceInfoTracker,
                          syncer::DeviceInfoTracker::Observer>
      device_info_observation_{this};
#if BUILDFLAG(IS_ANDROID)
  // The app coming back refreshes, as a window's activation does on the
  // desktop.
  std::unique_ptr<base::android::ApplicationStatusListener>
      app_status_listener_;
#else
  base::ScopedObservation<ProfileBrowserCollection, BrowserCollectionObserver>
      browser_observation_{this};
#endif
  bool observing_power_ = false;

  // Slice 7: the chain's limits on the server (docs/design/sync.md, "Slice
  // 7 notes"). Asked after sync cycles, at most every 30 minutes, every
  // minute while commits fail or a limit is in the way.
  void MaybeCheckQuota();
  void OnQuotaStatus(std::optional<QuotaStatus> status);
  SyncLimit GetLimit() const;
  std::unique_ptr<QuotaFetcher> quota_fetcher_;
  std::optional<QuotaStatus> quota_;
  base::TimeTicks last_quota_check_;

  base::WeakPtrFactory<CompassSyncService> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_BROWSER_SYNC_COMPASS_SYNC_SERVICE_H_
