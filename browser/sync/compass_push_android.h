// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_COMPASS_PUSH_ANDROID_H_
#define COMPASS_BROWSER_SYNC_COMPASS_PUSH_ANDROID_H_

#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "components/gcm_driver/gcm_app_handler.h"
#include "components/gcm_driver/instance_id/instance_id.h"
#include "url/gurl.h"

namespace gcm {
class GCMDriver;
}  // namespace gcm

namespace instance_id {
class InstanceIDDriver;
}  // namespace instance_id

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace compass::sync {

// FCM push for sent tabs on Android (docs/design/android.md, "Sync and
// push"; send-tab.md, "With push"). Gets an FCM registration token for
// Compass's Firebase project through Chromium's Instance ID (Play services),
// registers it with the sync server as iOS registers its APNs token
// (POST <server>/compass/push, platform "fcm"), and runs |on_wake| for the
// server's wake-ups. The message carries nothing: sync brings the tab, and
// Chrome's send-tab-to-self shows it.
class CompassPushAndroid : public gcm::GCMAppHandler {
 public:
  // Compass's Firebase project (its project number, the FCM sender ID).
  static const char kSenderId[];
  // The Instance ID app the token belongs to. The server puts it in its
  // messages as their "subtype" (server/push: FCMSubtype), which Chromium
  // routes them by.
  static const char kAppId[];

  using TokenGetter = base::RepeatingCallback<std::optional<std::string>()>;
  using GuidGetter = base::RepeatingCallback<std::string()>;

  CompassPushAndroid(gcm::GCMDriver* gcm_driver,
                     instance_id::InstanceIDDriver* instance_id_driver,
                     scoped_refptr<network::SharedURLLoaderFactory> loader,
                     const GURL& server_url,
                     TokenGetter token_getter,
                     GuidGetter guid_getter,
                     base::RepeatingClosure on_wake);
  CompassPushAndroid(const CompassPushAndroid&) = delete;
  CompassPushAndroid& operator=(const CompassPushAndroid&) = delete;
  ~CompassPushAndroid() override;

  // Listens for wake-ups and registers the token (again: a token can change,
  // and the server keeps one per device).
  void Start();

  // Tells the server to forget this device's registration (DELETE
  // <server>/compass/push), as the device leaves the chain. Best effort: the
  // request outlives this object, and a token the server keeps only wakes a
  // device that no longer syncs. Call it before the chain's credentials go.
  void Unregister();

  // <server>/compass/push?client_id=<guid>.
  static GURL MakePushUrl(const GURL& server_url, const std::string& guid);

  // gcm::GCMAppHandler:
  void ShutdownHandler() override;
  void OnStoreReset() override;
  void OnMessage(const std::string& app_id,
                 const gcm::IncomingMessage& message) override;
  void OnMessagesDeleted(const std::string& app_id) override;
  void OnSendError(
      const std::string& app_id,
      const gcm::GCMClient::SendErrorDetails& send_error_details) override;
  void OnSendAcknowledged(const std::string& app_id,
                          const std::string& message_id) override;

 private:
  void OnToken(const std::string& token, instance_id::InstanceID::Result result);
  void OnRegistered(std::optional<std::string> body);

  raw_ptr<gcm::GCMDriver> gcm_driver_;
  const raw_ptr<instance_id::InstanceIDDriver> instance_id_driver_;
  const scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const GURL server_url_;
  const TokenGetter token_getter_;
  const GuidGetter guid_getter_;
  const base::RepeatingClosure on_wake_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::WeakPtrFactory<CompassPushAndroid> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_BROWSER_SYNC_COMPASS_PUSH_ANDROID_H_
