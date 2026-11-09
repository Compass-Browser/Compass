// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_SYNC_BRIDGE_H_
#define COMPASS_BROWSER_ANDROID_SYNC_BRIDGE_H_

#include <jni.h>

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
  void Restart(JNIEnv* env);
  void SetDataTypeEnabled(JNIEnv* env, int32_t type, bool enabled);

  // sync::SyncController::Observer:
  void OnSyncControllerChanged() override;

 private:
  void OnDone(bool ok, std::string error);
  void OnStarted(bool ok, std::string error, std::vector<std::string> words);

  base::android::ScopedJavaGlobalRef<jobject> java_object_;
  raw_ptr<sync::SyncController> controller_;
  // The last start or join's error, and a new chain's recovery code.
  std::string error_;
  std::vector<std::string> new_words_;
  base::ScopedObservation<sync::SyncController,
                          sync::SyncController::Observer>
      observation_{this};
  base::WeakPtrFactory<SyncBridge> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_SYNC_BRIDGE_H_
