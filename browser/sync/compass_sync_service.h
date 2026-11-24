// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_COMPASS_SYNC_SERVICE_H_
#define COMPASS_BROWSER_SYNC_COMPASS_SYNC_SERVICE_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/power_monitor/power_observer.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
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
#include "compass/browser/sync/sync_settings.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/components/sync/chain.h"
#include "compass/components/sync/cloudflare_deployer.h"
#include "compass/components/sync/compass_account_linker.h"
#include "compass/components/sync/compass_device_remover.h"
#include "compass/components/sync/compass_sync_quota.h"
#include "compass/components/sync/login_client.h"
#include "compass/components/sync/pairing.h"
#include "compass/components/sync/profile_list.h"
#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/sync/service/sync_service_observer.h"
#include "components/sync_device_info/device_info_tracker.h"
#include "url/gurl.h"

class PrefService;
class Profile;

namespace content {
class WebContents;
}  // namespace content

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
#if BUILDFLAG(IS_ANDROID)
class CompassPushAndroid;
#endif
class SyncCredentials;
class CloudflareSignIn;

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
  // Makes a new profile for a space (AssignSpaceToProfile()). With the chain
  // running on this device, the profile is made in it, as the chain's
  // profiles sync brings are (CompassProfileCreator: its sync prefs are
  // written before it loads), so it syncs at once, without a restart, and
  // |loaded| runs with it once it's listed (null if that failed). Returns
  // false, without running |loaded|, when sync doesn't run here: the caller
  // makes a plain profile, which joins the chain from the next launch.
  static bool CreateProfileInChain(base::OnceCallback<void(Profile*)> loaded);

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
#if !BUILDFLAG(IS_ANDROID)
  void StartOnCloudflare(const std::string& access_token,
                         bool push_relay,
                         StartCallback callback) override;
  void StartOnCloudflareSignIn(bool push_relay,
                               StartCallback callback) override;
  void GetServerUpdate(ServerUpdateCallback callback) override;
  void UpdateServer(const std::string& access_token,
                    DoneCallback callback) override;
#endif
  void JoinWithInvite(const std::string& invite_link,
                      StartCallback callback) override;
  void GetServerPeople(PeopleCallback callback) override;
  void InviteFriend(const std::string& name, InviteCallback callback) override;
  void SetServerPushRelay(bool on, DoneCallback callback) override;
  void RemoveFromServer(const std::string& target,
                        DoneCallback callback) override;
  void JoinWithRecoveryCode(const std::string& server_url,
                            const std::string& words,
                            DoneCallback callback) override;
  void JoinWithPairingCode(const std::string& server_url,
                           const std::string& code) override;
  void AddDevice() override;
  void AnswerPairing(bool allow) override;
  void CancelPairing() override;
  void Leave() override;
  void StartLogin() override;
  void CancelLogin() override;
  void Restart() override;
  std::vector<SyncDevice> GetDevices() const override;
  void RemoveDevice(const std::string& guid, DoneCallback callback) override;
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
#if BUILDFLAG(IS_ANDROID)
  // Loads and lists the chain's profiles this device has that the space
  // directory doesn't list (ReconcileProfiles()).
  void ListChainProfiles();
#endif
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
#if BUILDFLAG(IS_ANDROID)
  void StartPush();
#endif

  void StartJoin(const GURL& server_url,
                 const std::string& code,
                 const std::string& device_name);
  void OnJoinFinished(const GURL& server_url,
                      PairingJoiner::Result result,
                      std::optional<Seed> seed);
  void SetPairing(PairingStatus status);
  void NotifyObservers();
  // The step a starting profile waits on (GetStatus() is kStarting then).
  StartingStep GetStartingStep() const;
  // Keeps |starting_step_| and |starting_since_| for the step under way,
  // and wakes the page once it has taken kStartingStuckAfter.
  void TrackStartingStep();
  // The seed is read: the engine gets the key, the passphrase follows.
  void OnSeedLoaded(const Seed& seed);
  // Profile account: links it to the chain's own on the server
  // (AccountLinker), then runs |done|, linked or not.
  void LinkAccount(const Seed& seed, base::OnceClosure done);
  // The server started or stopped refusing this account (401).
  void OnRefusedChanged();

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
  // While starting: the step, since when, and a timer for when it counts as
  // stuck (the page then offers a restart).
  std::optional<StartingStep> starting_step_;
  base::TimeTicks starting_since_;
  base::OneShotTimer starting_timer_;
  std::unique_ptr<AccountLinker> account_linker_;
  base::TimeTicks last_link_;
  base::OneShotTimer link_retry_timer_;

  std::unique_ptr<PairingHost> host_;
  std::unique_ptr<PairingJoiner> joiner_;
  std::unique_ptr<CompassSyncNotifier> notifier_;
#if BUILDFLAG(IS_ANDROID)
  std::unique_ptr<CompassPushAndroid> push_;
#endif
  PairingStatus pairing_;

  // Slice 5.
  std::optional<PendingJoin> pending_join_;
  // Home profile: the chain's profiles as its bridge last had them.
  std::vector<ChainProfile> chain_profiles_;
  bool chain_profiles_known_ = false;
  std::unique_ptr<CompassProfileCreator> profile_creator_;
  // CreateProfileInChain(): a creator per new profile, by its UUID, with
  // its callback and the profile once loaded.
  struct SpaceProfileCreation {
    SpaceProfileCreation();
    SpaceProfileCreation(SpaceProfileCreation&&);
    SpaceProfileCreation& operator=(SpaceProfileCreation&&);
    ~SpaceProfileCreation();
    std::unique_ptr<CompassProfileCreator> creator;
    base::OnceCallback<void(Profile*)> loaded;
    raw_ptr<Profile> profile = nullptr;
  };
  std::map<std::string, SpaceProfileCreation> space_profiles_;
  // A profile the chain's creator made (|uuid|): recorded in
  // compass.sync.profile_dirs, and the chain's list gets it.
  void OnChainProfileCreated(const std::string& uuid, Profile* profile);
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

  // Removing a device (docs/design/sync.md, "Removing a device").
  //
  // This profile's cache GUID (its DEVICE_INFO entry's), empty if unknown.
  std::string LocalGuid() const;
  // The account's other devices, as the page lists them, with what tells
  // them apart across the chain's profile accounts.
  std::vector<SignedDevice> GetSignedDevices() const;
  // Removes |guid| from this profile's account only.
  void RemoveDeviceFromAccount(const std::string& guid, DoneCallback callback);
  void OnDeviceRemovalResult(const std::string& guid,
                             DoneCallback callback,
                             DeviceRemover::Result result);
  // The server removed this device: stops asking it, says so.
  bool IsDeviceRemoved() const;
  void OnDeviceRemoved();
  std::unique_ptr<DeviceRemover> device_remover_;
  // Removed during this run: hidden until the tombstone syncs.
  std::set<std::string> removed_guids_;

  // Company sign-in (docs/design/sync.md, "Company sign-in (OIDC)").
  //
  // A sign-in that runs: the server's request, and who waits for it.
  struct LoginFlow {
    LoginFlow();
    ~LoginFlow();

    std::unique_ptr<LoginClient> client;
    // ok: the chain is signed in.
    DoneCallback done;
    LoginRequest request;
    // StartLogin() sent, and answered.
    bool requested = false;
    bool started = false;
  };
  // A client for the chain of |key| on |server_url| (a chain not saved
  // yet, a profile account).
  std::unique_ptr<LoginClient> MakeLoginClient(const GURL& server_url,
                                               const AuthKey& key);
  // The running chain's (credentials_'s token).
  LoginClient* GetLoginClient();
  // The running chain: asks the server whether it asks for sign-in (once a
  // run), then who the chain is signed in as. |force| skips the 30 s
  // spacing.
  void RefreshLogin(bool force);
  void OnServerLoginInfo(std::optional<ServerLoginInfo> info,
                         LoginError error);
  void OnAccountStatus(std::optional<AccountStatus> account, LoginError error);
  // A 401 from the notifier or the quota: maybe the chain must sign in.
  void OnAuthErrorHint();
  // The access client for the running chain's server; null without a
  // chain running.
  AccessClient* GetAccessClient();
  // ResetChain() once the server lets the new chain in.
  void ResetChainWithSeed(const GURL& server_url,
                          const Seed& seed,
                          StartCallback callback);
#if !BUILDFLAG(IS_ANDROID)
  // UpdateServer() with a token.
  void UpdateServerWithToken(const std::string& access_token,
                             DoneCallback callback);
#endif
  // StartNewChain() with a seed made already (StartOnCloudflare() deploys
  // the server for its chain first).
  void StartNewChainWithSeed(const GURL& url,
                             const Seed& seed,
                             StartCallback callback);
  // Before a chain is saved (StartNewChain(), JoinWithRecoveryCode()):
  // whether the server asks the chain of |seed| to sign in and it isn't.
  // Uses setup_client_.
  void CheckServerLogin(const GURL& server_url,
                        const Seed& seed,
                        base::OnceCallback<void(bool needed)> callback);
  // Runs a sign-in with |client|'s chain.
  void RunLogin(std::unique_ptr<LoginClient> client, DoneCallback done);
  void OnLoginDeviceName(std::string device_name);
  void OnLoginStarted(std::optional<LoginRequest> request, LoginError error);
  void OnLoginResult(LoginResult result);
  void FinishLogin(bool ok, std::string error);
  // The running chain signed in: the engine, the notifier and the profiles
  // go on.
  void OnChainSignedIn();
  // A profile account: its home linked it (or the chain signed in).
  void OnLinked();
  // Home profile: links the chain's profile accounts to its sign-in, the
  // pending ones (prefs::kLoginLinksPending) and, with |all|, every
  // profile of the chain here. Best effort: those that fail stay pending.
  void LinkProfiles(const GURL& server_url, const Seed& seed, bool all);
  void OnProfileLinked(int client_id, std::string uuid, LoginError error);
  // Home profile: links |uuid| now (a profile account found its chain
  // isn't signed in).
  void LinkProfileFromHome(const std::string& uuid);
  void OpenLoginPage(const GURL& url);
  // Closes the page's tab if it still shows |page_url|'s origin.
  void CloseLoginPage(const GURL& page_url);
  // "Sign in to Acme SSO".
  std::string SignInPhrase() const;

  // StartOnCloudflare(): the deploy that runs, and its progress line.
  std::unique_ptr<CloudflareDeployer> deployer_;
#if !BUILDFLAG(IS_ANDROID)
  // StartOnCloudflareSignIn(): the sign-in tab, until it gives a token.
  std::unique_ptr<CloudflareSignIn> cloudflare_sign_in_;
#endif
  // ResetChain() on a server Compass set up: the old chain links the new.
  std::unique_ptr<AccountLinker> reset_linker_;
  // The running chain's server's access list (invites), and the one a
  // JoinWithInvite() joins.
  std::unique_ptr<AccessClient> access_client_;
  std::unique_ptr<AccessClient> invite_client_;
  std::string server_setup_;

  std::unique_ptr<LoginClient> login_client_;
  // Checks before saving a chain.
  std::unique_ptr<LoginClient> setup_client_;
  // Links, by id, until they answer.
  std::map<int, std::unique_ptr<LoginClient>> oneshot_clients_;
  int next_client_id_ = 0;
  // The server's /compass/info, for |server_login_url_|.
  std::optional<ServerLoginInfo> server_login_;
  GURL server_login_url_;
  bool fetching_login_ = false;
  base::TimeTicks last_login_check_;
  // The running chain's /compass/account.
  std::optional<AccountStatus> account_;
  std::unique_ptr<LoginFlow> login_;
  // The last sign-in's failure, for the page.
  std::string login_error_;
  // A profile account asked its home for a link this run.
  bool link_requested_ = false;
#if !BUILDFLAG(IS_ANDROID)
  // The tab of the server's page; closed when the sign-in ends. (Android:
  // SyncBridge opens and closes it.)
  base::WeakPtr<content::WebContents> login_tab_;
#endif

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
