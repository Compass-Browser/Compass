// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/compass_push_android.h"

#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/values.h"
#include "components/gcm_driver/gcm_driver.h"
#include "components/gcm_driver/instance_id/instance_id_driver.h"
#include "net/base/url_util.h"
#include "net/http/http_request_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"

namespace compass::sync {

namespace {

// The app the server pushes to (server/push: its default topic, the iOS
// bundle ID and the Android package name alike).
constexpr char kTopic[] = "it.cipi1965.compass";

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_sync_push_android", R"(
        semantics {
          sender: "Compass sync"
          description:
            "Tells the user's sync server this device's FCM registration "
            "token, so the server can wake the device when another of the "
            "user's devices sends it a tab."
          trigger: "When sync starts on the device, while sync is on."
          data: "The chain's signed token, the device's sync ID and its FCM "
                "registration token."
          destination: OTHER
          destination_other: "The self-hosted sync server the user set."
        }
        policy {
          cookies_allowed: NO
          setting: "Only while the user syncs through a Compass sync server."
          policy_exception_justification: "Not implemented."
        })");

}  // namespace

const char CompassPushAndroid::kSenderId[] = "793397534280";
const char CompassPushAndroid::kAppId[] = "it.cipi1965.compass.sync-push";

CompassPushAndroid::CompassPushAndroid(
    gcm::GCMDriver* gcm_driver,
    instance_id::InstanceIDDriver* instance_id_driver,
    scoped_refptr<network::SharedURLLoaderFactory> loader,
    const GURL& server_url,
    TokenGetter token_getter,
    GuidGetter guid_getter,
    base::RepeatingClosure on_wake)
    : gcm_driver_(gcm_driver),
      instance_id_driver_(instance_id_driver),
      loader_factory_(std::move(loader)),
      server_url_(server_url),
      token_getter_(std::move(token_getter)),
      guid_getter_(std::move(guid_getter)),
      on_wake_(std::move(on_wake)) {}

CompassPushAndroid::~CompassPushAndroid() {
  if (gcm_driver_) {
    gcm_driver_->RemoveAppHandler(kAppId);
  }
}

// static
GURL CompassPushAndroid::MakePushUrl(const GURL& server_url,
                                     const std::string& guid) {
  std::string path = server_url.GetPath();
  while (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  path += "/compass/push";
  GURL::Replacements replacements;
  replacements.SetPathStr(path);
  replacements.ClearQuery();
  replacements.ClearRef();
  return net::AppendQueryParameter(server_url.ReplaceComponents(replacements),
                                   "client_id", guid);
}

void CompassPushAndroid::Start() {
  if (gcm_driver_ && !gcm_driver_->GetAppHandler(kAppId)) {
    gcm_driver_->AddAppHandler(kAppId, this);
  }
  // High priority wake-ups go to the browser at once, not through the
  // background task scheduler.
  instance_id_driver_->GetInstanceID(kAppId)->GetToken(
      kSenderId, instance_id::kGCMScope, base::TimeDelta(),
      {instance_id::InstanceID::Flags::kBypassScheduler},
      base::BindOnce(&CompassPushAndroid::OnToken,
                     weak_factory_.GetWeakPtr()));
}

void CompassPushAndroid::OnToken(const std::string& token,
                                 instance_id::InstanceID::Result result) {
  if (result != instance_id::InstanceID::SUCCESS || token.empty()) {
    LOG(WARNING) << "Compass push: no FCM token (" << static_cast<int>(result)
                 << ")";
    return;
  }
  const std::optional<std::string> access = token_getter_.Run();
  const std::string guid = guid_getter_.Run();
  if (!access || guid.empty() || loader_) {
    return;
  }
  base::DictValue body;
  body.Set("token", token);
  body.Set("platform", "fcm");
  body.Set("topic", kTopic);
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = MakePushUrl(server_url_, guid);
  request->method = "POST";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader(net::HttpRequestHeaders::kAuthorization,
                             base::StrCat({"Bearer ", *access}));
  loader_ =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  loader_->AttachStringForUpload(base::WriteJson(body).value_or("{}"),
                                 "application/json");
  loader_->SetTimeoutDuration(base::Seconds(30));
  loader_->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&CompassPushAndroid::OnRegistered,
                     weak_factory_.GetWeakPtr()),
      /*max_body_size=*/4096);
}

void CompassPushAndroid::OnRegistered(std::optional<std::string> body) {
  // 204 gives an empty body; an HTTP error gives none.
  if (!body) {
    LOG(WARNING) << "Compass push: the server refused the FCM token ("
                 << loader_->NetError() << ")";
  }
  loader_.reset();
}

void CompassPushAndroid::ShutdownHandler() {
  gcm_driver_ = nullptr;
}

void CompassPushAndroid::OnStoreReset() {}

void CompassPushAndroid::OnMessage(const std::string& app_id,
                                   const gcm::IncomingMessage& message) {
  on_wake_.Run();
}

void CompassPushAndroid::OnMessagesDeleted(const std::string& app_id) {
  // Wake-ups were dropped: sync anyway.
  on_wake_.Run();
}

void CompassPushAndroid::OnSendError(
    const std::string& app_id,
    const gcm::GCMClient::SendErrorDetails& send_error_details) {}

void CompassPushAndroid::OnSendAcknowledged(const std::string& app_id,
                                            const std::string& message_id) {}

}  // namespace compass::sync
