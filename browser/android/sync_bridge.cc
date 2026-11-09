// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/sync_bridge.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/sync/compass_sync_service.h"
#include "compass/browser/sync/compass_sync_service_factory.h"

// Must come after the class declaration.
#include "compass/android/jni_headers/SyncBridge_jni.h"

namespace compass {

namespace {

const char* StateName(sync::SyncState state) {
  switch (state) {
    case sync::SyncState::kOff:
      return "off";
    case sync::SyncState::kNeedsRestart:
      return "needs_restart";
    case sync::SyncState::kStarting:
      return "starting";
    case sync::SyncState::kOn:
      return "on";
    case sync::SyncState::kError:
      return "error";
  }
  return "off";
}

const char* StepName(sync::PairingStep step) {
  switch (step) {
    case sync::PairingStep::kNone:
      return "none";
    case sync::PairingStep::kWorking:
      return "working";
    case sync::PairingStep::kShowingCode:
      return "showing_code";
    case sync::PairingStep::kConfirm:
      return "confirm";
    case sync::PairingStep::kWaitingForApproval:
      return "waiting";
    case sync::PairingStep::kDone:
      return "done";
    case sync::PairingStep::kFailed:
      return "failed";
  }
  return "none";
}

}  // namespace

SyncBridge::SyncBridge(JNIEnv* env,
                       const base::android::JavaRef<jobject>& java_object,
                       Profile* profile)
    : java_object_(env, java_object),
      controller_(sync::CompassSyncServiceFactory::GetForProfile(profile)) {
  if (controller_) {
    observation_.Observe(controller_.get());
  }
}

SyncBridge::~SyncBridge() = default;

void SyncBridge::Destroy(JNIEnv* env) {
  delete this;
}

std::string SyncBridge::Snapshot(JNIEnv* env) {
  base::DictValue snapshot;
  if (!controller_) {
    snapshot.Set("state", "unavailable");
    return base::WriteJson(snapshot).value_or("{}");
  }
  const sync::SyncStatus status = controller_->GetStatus();
  snapshot.Set("state", StateName(status.state));
  snapshot.Set("server", status.server_url);
  snapshot.Set("message", status.message);
  const sync::PairingStatus pairing = controller_->GetPairingStatus();
  base::DictValue pairing_dict;
  pairing_dict.Set("step", StepName(pairing.step));
  pairing_dict.Set("code", pairing.code);
  pairing_dict.Set("device", pairing.device_name);
  pairing_dict.Set("message", pairing.message);
  snapshot.Set("pairing", std::move(pairing_dict));
  base::ListValue devices;
  for (const sync::SyncDevice& device : controller_->GetDevices()) {
    base::DictValue dict;
    dict.Set("name", device.name);
    dict.Set("kind", device.kind);
    dict.Set("this", device.this_device);
    devices.Append(std::move(dict));
  }
  snapshot.Set("devices", std::move(devices));
  base::ListValue types;
  for (const sync::SyncDataTypeState& type : controller_->GetDataTypes()) {
    base::DictValue dict;
    dict.Set("type", static_cast<int>(type.type));
    dict.Set("enabled", type.enabled);
    types.Append(std::move(dict));
  }
  snapshot.Set("types", std::move(types));
  snapshot.Set("error", error_);
  base::ListValue words;
  for (const std::string& word : new_words_) {
    words.Append(word);
  }
  snapshot.Set("new_words", std::move(words));
  return base::WriteJson(snapshot).value_or("{}");
}

void SyncBridge::StartNewChain(JNIEnv* env, const std::string& server_url) {
  if (!controller_) {
    return;
  }
  error_.clear();
  controller_->StartNewChain(server_url,
                             base::BindOnce(&SyncBridge::OnStarted,
                                            weak_factory_.GetWeakPtr()));
}

void SyncBridge::JoinWithPairingCode(JNIEnv* env,
                                     const std::string& server_url,
                                     const std::string& code) {
  if (controller_) {
    error_.clear();
    controller_->JoinWithPairingCode(server_url, code);
  }
}

void SyncBridge::JoinWithRecoveryCode(JNIEnv* env,
                                      const std::string& server_url,
                                      const std::string& words) {
  if (!controller_) {
    return;
  }
  error_.clear();
  controller_->JoinWithRecoveryCode(
      server_url, words,
      base::BindOnce(&SyncBridge::OnDone, weak_factory_.GetWeakPtr()));
}

void SyncBridge::AddDevice(JNIEnv* env) {
  if (controller_) {
    controller_->AddDevice();
  }
}

void SyncBridge::AnswerPairing(JNIEnv* env, bool allow) {
  if (controller_) {
    controller_->AnswerPairing(allow);
  }
}

void SyncBridge::CancelPairing(JNIEnv* env) {
  if (controller_) {
    controller_->CancelPairing();
  }
}

void SyncBridge::Leave(JNIEnv* env) {
  if (controller_) {
    controller_->Leave();
  }
}

void SyncBridge::Restart(JNIEnv* env) {
  if (controller_) {
    controller_->Restart();
  }
}

void SyncBridge::SetDataTypeEnabled(JNIEnv* env, int32_t type, bool enabled) {
  if (controller_ && type >= 0 &&
      type <= static_cast<int>(sync::SyncDataType::kSpaces)) {
    controller_->SetDataTypeEnabled(static_cast<sync::SyncDataType>(type),
                                    enabled);
  }
}

void SyncBridge::OnSyncControllerChanged() {
  Java_SyncBridge_onChanged(jni_zero::AttachCurrentThread(), java_object_);
}

void SyncBridge::OnDone(bool ok, std::string error) {
  error_ = ok ? std::string() : std::move(error);
  OnSyncControllerChanged();
}

void SyncBridge::OnStarted(bool ok,
                           std::string error,
                           std::vector<std::string> words) {
  error_ = ok ? std::string() : std::move(error);
  new_words_ = std::move(words);
  OnSyncControllerChanged();
}

static int64_t JNI_SyncBridge_Init(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& caller,
    Profile* profile) {
  return reinterpret_cast<int64_t>(new SyncBridge(env, caller, profile));
}

}  // namespace compass

DEFINE_JNI(SyncBridge)
