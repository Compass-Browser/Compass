// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_SYNC_CONTROLLER_H_
#define COMPASS_BROWSER_SYNC_SYNC_CONTROLLER_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "compass/components/sync/access_client.h"
#include "compass/components/sync/profile_list.h"

class GURL;

namespace compass::sync {

enum class SyncState {
  // Not in a chain.
  kOff,
  // In a chain from the next launch on (or out of it, after leaving): the
  // sync service picks its mode when the profile loads.
  kNeedsRestart,
  kStarting,
  kOn,
  kError,
  // The server asks the chain to sign in (docs/design/sync.md, "Company
  // sign-in (OIDC)"): sync waits for StartLogin().
  kLoginRequired,
};

// A server limit in the way of syncing (docs/design/sync.md, "Slice 7
// notes"), from the server's quota status and the engine's throttling.
enum class SyncLimit {
  kNone,
  // The chain's history quota is used up: new visits stay on this device.
  kHistoryFull,
  // The server refuses new data for the chain (items or storage).
  kServerFull,
  // The server holds this device back (too many devices, or requests).
  kThrottled,
};

// The server's optional company sign-in (docs/design/sync.md, "Company
// sign-in (OIDC)").
struct SyncLogin {
  // The server asks chains to sign in, with |provider_name| ("Acme SSO";
  // may be empty).
  bool server_requires = false;
  std::string provider_name;
  // Signed in as (empty if not, or not known).
  std::string user_name;
  std::string user_email;
  // A sign-in runs: the user finishes it on the server's page, which
  // shows |user_code|; |url| is that page.
  bool in_progress = false;
  std::string user_code;
  std::string url;
};

struct SyncStatus {
  SyncState state = SyncState::kOff;
  std::string server_url;
  // One line for the page: last synced, or what's wrong.
  std::string message;
  SyncLimit limit = SyncLimit::kNone;
  // The server says this device was removed from the chain on another device
  // (docs/design/sync.md, "Removing a device"): kError, and the only way on
  // is leaving (then joining again).
  bool device_removed = false;
  // kStarting for a long time on one step: the page offers a restart next to
  // the message, which says so (StartingStatus()).
  bool offer_restart = false;
  SyncLogin login;
  // Off: while StartOnCloudflare() runs, what it's doing ("Uploading the
  // sync server…"). Empty otherwise.
  std::string server_setup;
};

enum class PairingStep {
  kNone,
  // Talking to the server.
  kWorking,
  // A: shows |code| and waits for the other device.
  kShowingCode,
  // A: asks whether to add |device_name|.
  kConfirm,
  // B: proved the code, waits for A's answer.
  kWaitingForApproval,
  kDone,
  kFailed,
};

struct PairingStatus {
  PairingStep step = PairingStep::kNone;
  std::string code;
  std::string device_name;
  std::string message;
  // kShowingCode: when the code stops working (the server's slot
  // expires_in after it opened). Null otherwise.
  base::Time expires_at;
};

// The switches on chrome://compass-sync, one per kind of data the user
// sees (docs/design/sync.md, "UI"). All but kSpaces are upstream
// UserSelectableTypes; kSpaces is COMPASS_SPACES.
enum class SyncDataType {
  kPasswords,
  kHistory,
  kBookmarks,
  kAutofill,
  kExtensions,
  kSettings,
  kReadingList,
  kOpenTabs,
  kSpaces,
};

struct SyncDataTypeState {
  SyncDataType type = SyncDataType::kPasswords;
  bool enabled = true;
};

// A device of the chain, from DEVICE_INFO.
struct SyncDevice {
  // Its cache GUID (DeviceInfo::guid()), which RemoveDevice() takes.
  std::string guid;
  std::string name;
  // "Mac", "Windows PC", "Android phone"...
  std::string kind;
  // When the device last said it's alive (null if unknown).
  base::Time last_seen;
  bool this_device = false;
};

// Joining a chain on a device with other profiles: how each of them syncs.
// See docs/design/sync.md, "Profiles".
struct ProfileMappingStatus {
  ProfileMappingStatus();
  ProfileMappingStatus(const ProfileMappingStatus&);
  ProfileMappingStatus& operator=(const ProfileMappingStatus&);
  ~ProfileMappingStatus();

  // Whether the join waits for the user to confirm a mapping.
  bool needed = false;
  // The chain's profiles; the home one is the joining profile's.
  std::vector<ChainProfile> chain;
  // This device's other profiles that can sync.
  std::vector<LocalProfile> local;
  // The default: same name, else a new profile of the chain.
  ProfileMapping proposed;
};

// What chrome://compass-sync drives: CompassSyncService, or a fake in tests.
class SyncController {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // The status or the pairing changed.
    virtual void OnSyncControllerChanged() = 0;
  };

  using StartCallback = base::OnceCallback<
      void(bool ok, std::string error, std::vector<std::string> words)>;
  using DoneCallback = base::OnceCallback<void(bool ok, std::string error)>;

  virtual ~SyncController() = default;

  virtual void AddObserver(Observer* observer) = 0;
  virtual void RemoveObserver(Observer* observer) = 0;

  virtual SyncStatus GetStatus() const = 0;
  virtual PairingStatus GetPairingStatus() const = 0;

  // Off: makes a chain on |server_url|; |words| is its recovery code. On a
  // server that asks chains to sign in, the sign-in runs first (see
  // StartLogin()), and the callback waits for it.
  virtual void StartNewChain(const std::string& server_url,
                             StartCallback callback) = 0;
  // Off: sets up the user's own server in their Cloudflare account with
  // |access_token| (a Cloudflare API token that can edit Workers), owned by
  // a new chain, then starts that chain on it as StartNewChain() does
  // (docs/design/sync.md, "A free server on Cloudflare"). Progress shows in
  // GetStatus().server_setup. Default: fails.
  virtual void StartOnCloudflare(const std::string& access_token,
                                 StartCallback callback);
  // Off: as StartOnCloudflare(), with an access token from "Sign in with
  // Cloudflare" in a new tab instead of a pasted one. Progress shows in
  // GetStatus().server_setup. Default: fails.
  virtual void StartOnCloudflareSignIn(StartCallback callback);
  // Off: makes a new chain on the server of an invite link
  // (https://<server>/invite#<code>): joins the server's access list with
  // the code, then starts the chain as StartNewChain() does
  // (docs/design/sync.md, "Inviting friends"). Default: fails.
  virtual void JoinWithInvite(const std::string& invite_link,
                              StartCallback callback);
  // On: the friends on the server's access list and the open invites, if
  // the server has a list and this chain owns it; nullopt otherwise.
  // Default: nullopt.
  using PeopleCallback =
      base::OnceCallback<void(std::optional<ServerPeople> people)>;
  virtual void GetServerPeople(PeopleCallback callback);
  // On, owner: a single-use invite for a friend (|name| is a reminder for
  // the owner, may be empty); |link| is what to send them.
  using InviteCallback = base::OnceCallback<
      void(bool ok, std::string error, std::string link)>;
  virtual void InviteFriend(const std::string& name, InviteCallback callback);
  // On, owner: removes a friend (their chain ID) or revokes an open invite
  // (its code).
  virtual void RemoveFromServer(const std::string& target,
                                DoneCallback callback);
  // Off: joins the chain of a recovery code. As StartNewChain(), signs the
  // chain in first if the server asks and it isn't.
  virtual void JoinWithRecoveryCode(const std::string& server_url,
                                    const std::string& words,
                                    DoneCallback callback) = 0;
  // Off: joins through a device already syncing (pairing, B's side).
  // Progress shows in GetPairingStatus().
  virtual void JoinWithPairingCode(const std::string& server_url,
                                   const std::string& code) = 0;
  // On: lets another device join (pairing, A's side).
  virtual void AddDevice() = 0;
  // After PairingStep::kConfirm.
  virtual void AnswerPairing(bool allow) = 0;
  virtual void CancelPairing() = 0;
  // On: leaves the chain; local data stays.
  virtual void Leave() = 0;
  // kLoginRequired: signs the chain in. Opens the server's page in a tab and
  // waits for the user to finish there; progress shows in GetStatus().login.
  virtual void StartLogin() = 0;
  // Ends a sign-in that runs (this one, or the one StartNewChain() or
  // JoinWithRecoveryCode() runs first on a server that asks for it: they
  // then fail).
  virtual void CancelLogin() = 0;
  virtual void Restart() = 0;

  // On: the chain's devices, this one included.
  virtual std::vector<SyncDevice> GetDevices() const = 0;
  // On: removes another device of the chain (|guid| from GetDevices()) on
  // the server; it stops syncing and leaves the list. Its data stays on it,
  // and it can join again. Not this device (fails).
  virtual void RemoveDevice(const std::string& guid, DoneCallback callback) = 0;
  // The switches, in the page's order. Kept while sync is off.
  virtual std::vector<SyncDataTypeState> GetDataTypes() const = 0;
  virtual void SetDataTypeEnabled(SyncDataType type, bool enabled) = 0;
  // On: the chain's 12 words, or empty if the seed isn't loaded. The page
  // asks the user to authenticate (Touch ID or the Mac's password) first.
  virtual std::vector<std::string> GetRecoveryCode() const = 0;
  // On: replaces the chain with a new one (new seed, so new keys and a new
  // account on the same server); |words| is its recovery code. This
  // device's data goes up again after a restart; the other devices must
  // join again.
  virtual void ResetChain(StartCallback callback) = 0;

  // Slice 5. While joining (pairing gave the chain's profiles), or after
  // joining by recovery code once the first sync brought them: the mapping
  // to confirm. Default: none needed.
  virtual ProfileMappingStatus GetProfileMapping() const;
  // Confirms a mapping: |mapping| has one entry per local profile of
  // GetProfileMapping(). Fails (and changes nothing) if none is needed or it
  // isn't valid.
  virtual void SetProfileMapping(const ProfileMapping& mapping,
                                 DoneCallback callback);
};

// A server URL as typed, or an invalid GURL: http or https, with a host.
GURL ParseServerUrl(std::string_view text);

}  // namespace compass::sync

#endif  // COMPASS_BROWSER_SYNC_SYNC_CONTROLLER_H_
