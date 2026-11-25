// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_SYNC_BRIDGE_H_
#define COMPASS_BROWSER_ANDROID_SYNC_BRIDGE_H_

#include <jni.h>

#include <optional>
#include <string>
#include <vector>

#include "base/android/scoped_java_ref.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "compass/browser/sync/sync_controller.h"

class Profile;

namespace compass {

// The native half of it.cipi1965.compass.sync.SyncBridge: the sync and
// pairing screen's view of the profile's CompassSyncService (its
// SyncController, as chrome://compass-sync uses on the desktop).
class SyncBridge : public sync::SyncController::Observer {
 public:
  SyncBridge(JNIEnv* env,
             const base::android::JavaRef<jobject>& java_object,
             Profile* profile);
  SyncBridge(const SyncBridge&) = delete;
  SyncBridge& operator=(const SyncBridge&) = delete;
  ~SyncBridge() override;

  void Destroy(JNIEnv* env);
  // Status, pairing, devices, data types and the last error, as JSON.
  std::string Snapshot(JNIEnv* env);
  void StartNewChain(JNIEnv* env, const std::string& server_url);
  void JoinWithPairingCode(JNIEnv* env,
                           const std::string& server_url,
                           const std::string& code);
  void JoinWithRecoveryCode(JNIEnv* env,
                            const std::string& server_url,
                            const std::string& words);
  void AddDevice(JNIEnv* env);
  void AnswerPairing(JNIEnv* env, bool allow);
  void CancelPairing(JNIEnv* env);
  void Leave(JNIEnv* env);
  // Removes another device of the chain; how it went shows in the
  // snapshot's "removing" and "remove_error".
  void RemoveDevice(JNIEnv* env, const std::string& guid);
  void StartLogin(JNIEnv* env);
  void CancelLogin(JNIEnv* env);
  void Restart(JNIEnv* env);
  void SetDataTypeEnabled(JNIEnv* env, int32_t type, bool enabled);
  // On: the chain's 12 words (empty if the seed isn't loaded). Java asks
  // for the device's screen lock first, as the desktop's page asks
  // device_reauth; the words stay out of the snapshot.
  std::vector<std::string> GetRecoveryCode(JNIEnv* env);
  // "I saved it": a new chain's words are shown once.
  void ClearNewWords(JNIEnv* env);

  // A free server on Cloudflare (docs/design/sync.md, "A free server on
  // Cloudflare"): with a pasted API token, or "Sign in with Cloudflare",
  // whose tab the service opens (the screen steps aside meanwhile: Java's
  // cloudflareSignIn()). Progress shows in the snapshot's "server_setup",
  // then as StartNewChain()'s.
  void StartOnCloudflare(JNIEnv* env,
                         const std::string& token,
                         bool push_relay);
  void StartOnCloudflareSignIn(JNIEnv* env, bool push_relay);
  // "I have an invite" (docs/design/sync.md, "Inviting friends").
  void JoinWithInvite(JNIEnv* env, const std::string& link);
  // The owner's "People on your server": asks the server again for its
  // people, then for an update (the snapshot's "people" and
  // "server_update"). Also when sync comes on for a server.
  void LoadPeople(JNIEnv* env);
  // A single-use invite link (the snapshot's "invite_link").
  void InviteFriend(JNIEnv* env, const std::string& name);
  // Removes a friend (their chain) or revokes an open invite (its code).
  void RemoveFromServer(JNIEnv* env, const std::string& target);
  void SetServerPushRelay(JNIEnv* env, bool on);
  // "A newer version of your server is ready": with a token, or "Update
  // with Cloudflare" (an empty one) after signing in in a tab.
  void UpdateServer(JNIEnv* env, const std::string& token);

  // sync::SyncController::Observer:
  void OnSyncControllerChanged() override;

 private:
  void OnDone(bool ok, std::string error);
  void OnStarted(bool ok, std::string error, std::vector<std::string> words);
  void OnDeviceRemoved(bool ok, std::string error);
  // Asks for the people (and then an update) if sync is on.
  void LoadPeopleNow();
  // Sync came on for a server the people weren't asked for: they are.
  void MaybeLoadPeople();
  void OnPeople(int generation, std::optional<sync::ServerPeople> people);
  void OnServerUpdate(int generation,
                      std::optional<sync::SyncController::ServerUpdate> update);
  void OnInvite(bool ok, std::string error, std::string link);
  void OnPeopleDone(bool ok, std::string error);
  void OnPushRelaySet(bool ok, std::string error);
  void OnServerUpdated(bool ok, std::string error);

  base::android::ScopedJavaGlobalRef<jobject> java_object_;
  raw_ptr<sync::SyncController> controller_;
  // The last start or join's error, and a new chain's recovery code.
  std::string error_;
  std::vector<std::string> new_words_;
  // A removal under way, and the last one's error.
  bool removing_ = false;
  std::string remove_error_;
  // Company sign-in: this screen started one (so opens its page), and the
  // page it opened.
  bool opens_login_ = false;
  std::string opened_login_url_;
  // "Sign in with Cloudflare" from this screen: its tab is open (the
  // service's status says so), so the screen stepped aside.
  bool opens_cloudflare_ = false;
  bool cloudflare_open_ = false;
  // People on the server (an owner's; nullopt: none to show), for the
  // server |people_for_|; a newer load wins (|people_generation_|).
  std::optional<sync::ServerPeople> people_;
  std::optional<sync::SyncController::ServerUpdate> server_update_;
  std::string people_for_;
  int people_generation_ = 0;
  // The last people action's error, the last invite link made (until sync
  // is off), whether the push switch waits for the server, and the last
  // update's error.
  std::string people_error_;
  std::string invite_link_;
  bool people_busy_ = false;
  std::string update_error_;
  base::ScopedObservation<sync::SyncController,
                          sync::SyncController::Observer>
      observation_{this};
  base::WeakPtrFactory<SyncBridge> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_SYNC_BRIDGE_H_
