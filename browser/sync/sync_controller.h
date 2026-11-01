// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_SYNC_CONTROLLER_H_
#define COMPASS_BROWSER_SYNC_SYNC_CONTROLLER_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
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
};

struct SyncStatus {
  SyncState state = SyncState::kOff;
  std::string server_url;
  // One line for the page: last synced, or what's wrong.
  std::string message;
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
};

// The switches on chrome://compass-sync, one per kind of data the user
// sees (docs/design/sync.md, "Slice 4 notes"). All but kSpaces are upstream
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
  std::string name;
  // "Mac", "Windows PC", "Android phone"...
  std::string kind;
  // When the device last said it's alive (null if unknown).
  base::Time last_seen;
  bool this_device = false;
};

// Joining a chain on a device with other profiles (slice 5): how each of them
// syncs. See docs/design/sync.md, "Slice 5 notes".
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

  // Off: makes a chain on |server_url|; |words| is its recovery code.
  virtual void StartNewChain(const std::string& server_url,
                             StartCallback callback) = 0;
  // Off: joins the chain of a recovery code.
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
  virtual void Restart() = 0;

  // On: the chain's devices, this one included.
  virtual std::vector<SyncDevice> GetDevices() const = 0;
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
