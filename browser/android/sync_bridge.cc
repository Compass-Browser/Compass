// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/sync_bridge.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/components/sync/access_client.h"
#include "url/gurl.h"
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
    case sync::SyncState::kLoginRequired:
      return "login_required";
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
  snapshot.Set("removed", status.device_removed);
  base::DictValue login;
  login.Set("required", status.login.server_requires);
  login.Set("provider", status.login.provider_name);
  login.Set("user_name", status.login.user_name);
  login.Set("user_email", status.login.user_email);
  login.Set("in_progress", status.login.in_progress);
  login.Set("code", status.login.user_code);
  snapshot.Set("login", std::move(login));
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
    dict.Set("guid", device.guid);
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
  snapshot.Set("removing", removing_);
  snapshot.Set("remove_error", remove_error_);
  base::ListValue words;
  for (const std::string& word : new_words_) {
    words.Append(word);
  }
  snapshot.Set("new_words", std::move(words));
  // A free server on Cloudflare, being set up or updated.
  snapshot.Set("server_setup", status.server_setup);
  snapshot.Set("cloudflare_sign_in", status.cloudflare_sign_in);
  // The owner's people (docs/design/sync.md, "Inviting friends"), as the
  // desktop's page shows them.
  if (people_) {
    base::DictValue people;
    base::ListValue members;
    for (const sync::ServerMember& member : people_->members) {
      members.Append(
          base::DictValue()
              .Set("chain", member.chain)
              .Set("name", member.name)
              .Set("added_ms", member.added.InMillisecondsFSinceUnixEpoch())
              .Set("linked", member.linked));
    }
    people.Set("members", std::move(members));
    base::ListValue invites;
    for (const sync::ServerInvite& invite : people_->invites) {
      invites.Append(
          base::DictValue()
              .Set("code", invite.code)
              .Set("name", invite.name)
              .Set("expires_ms",
                   invite.expires.InMillisecondsFSinceUnixEpoch()));
    }
    people.Set("invites", std::move(invites));
    // A server from before version 3 has no such setting, and one with
    // push off for good has no relay: no switch.
    people.Set("push_relay_available", people_->push_relay.has_value());
    people.Set("push_relay", people_->push_relay.value_or(false));
    people.Set("push_relay_host", GURL(people_->push_relay_url).host());
    snapshot.Set("people", std::move(people));
  }
  if (server_update_) {
    snapshot.Set("server_update",
                 base::DictValue()
                     .Set("current", server_update_->current)
                     .Set("latest", server_update_->latest));
  }
  snapshot.Set("people_error", people_error_);
  snapshot.Set("invite_link", invite_link_);
  snapshot.Set("people_busy", people_busy_);
  snapshot.Set("update_error", update_error_);
  return base::WriteJson(snapshot).value_or("{}");
}

void SyncBridge::StartNewChain(JNIEnv* env, const std::string& server_url) {
  if (!controller_) {
    return;
  }
  error_.clear();
  // A server that asks for sign-in runs it first: this screen opens it.
  opens_login_ = true;
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
  opens_login_ = true;
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

void SyncBridge::RemoveDevice(JNIEnv* env, const std::string& guid) {
  if (!controller_ || removing_) {
    return;
  }
  removing_ = true;
  remove_error_.clear();
  OnSyncControllerChanged();
  controller_->RemoveDevice(guid, base::BindOnce(&SyncBridge::OnDeviceRemoved,
                                                 weak_factory_.GetWeakPtr()));
}

void SyncBridge::OnDeviceRemoved(bool ok, std::string error) {
  removing_ = false;
  remove_error_ = ok ? std::string() : std::move(error);
  OnSyncControllerChanged();
}

void SyncBridge::StartLogin(JNIEnv* env) {
  if (!controller_) {
    return;
  }
  opens_login_ = true;
  // Again while it runs: the page opens again.
  opened_login_url_.clear();
  controller_->StartLogin();
  OnSyncControllerChanged();
}

void SyncBridge::CancelLogin(JNIEnv* env) {
  if (controller_) {
    controller_->CancelLogin();
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

std::vector<std::string> SyncBridge::GetRecoveryCode(JNIEnv* env) {
  if (!controller_) {
    return {};
  }
  return controller_->GetRecoveryCode();
}

void SyncBridge::ClearNewWords(JNIEnv* env) {
  new_words_.clear();
}

void SyncBridge::StartOnCloudflare(JNIEnv* env,
                                   const std::string& token,
                                   bool push_relay) {
  if (!controller_) {
    return;
  }
  error_.clear();
  controller_->StartOnCloudflare(
      token, push_relay,
      base::BindOnce(&SyncBridge::OnStarted, weak_factory_.GetWeakPtr()));
}

void SyncBridge::StartOnCloudflareSignIn(JNIEnv* env, bool push_relay) {
  if (!controller_) {
    return;
  }
  error_.clear();
  // Before the call: the service says its tab opens as it starts.
  opens_cloudflare_ = true;
  controller_->StartOnCloudflareSignIn(
      push_relay,
      base::BindOnce(&SyncBridge::OnStarted, weak_factory_.GetWeakPtr()));
}

void SyncBridge::JoinWithInvite(JNIEnv* env, const std::string& link) {
  if (!controller_) {
    return;
  }
  error_.clear();
  opens_login_ = true;
  controller_->JoinWithInvite(
      link, base::BindOnce(&SyncBridge::OnStarted, weak_factory_.GetWeakPtr()));
}

void SyncBridge::LoadPeople(JNIEnv* env) {
  LoadPeopleNow();
}

void SyncBridge::LoadPeopleNow() {
  if (!controller_) {
    return;
  }
  const sync::SyncStatus status = controller_->GetStatus();
  if (status.state != sync::SyncState::kOn || status.device_removed) {
    return;
  }
  people_for_ = status.server_url;
  const int generation = ++people_generation_;
  controller_->GetServerPeople(base::BindOnce(
      &SyncBridge::OnPeople, weak_factory_.GetWeakPtr(), generation));
}

void SyncBridge::MaybeLoadPeople() {
  const sync::SyncStatus status = controller_->GetStatus();
  if (status.state == sync::SyncState::kOn && !status.device_removed) {
    if (people_for_ != status.server_url) {
      LoadPeopleNow();
    }
    return;
  }
  // Off (left, reset) or removed: nothing to show, and they're asked again
  // when sync is on. (Starting and errors keep what was shown.)
  if (status.state == sync::SyncState::kOff || status.device_removed) {
    people_for_.clear();
    people_.reset();
    server_update_.reset();
    ++people_generation_;
    invite_link_.clear();
    people_error_.clear();
    update_error_.clear();
  }
}

void SyncBridge::OnPeople(int generation,
                          std::optional<sync::ServerPeople> people) {
  if (generation != people_generation_) {
    return;
  }
  people_ = std::move(people);
  if (!people_) {
    server_update_.reset();
    OnSyncControllerChanged();
    return;
  }
  // The owner's offer to update a server Compass set up on Cloudflare.
  controller_->GetServerUpdate(base::BindOnce(
      &SyncBridge::OnServerUpdate, weak_factory_.GetWeakPtr(), generation));
  OnSyncControllerChanged();
}

void SyncBridge::OnServerUpdate(
    int generation,
    std::optional<sync::SyncController::ServerUpdate> update) {
  if (generation != people_generation_) {
    return;
  }
  server_update_ = std::move(update);
  OnSyncControllerChanged();
}

void SyncBridge::InviteFriend(JNIEnv* env, const std::string& name) {
  if (!controller_) {
    return;
  }
  people_error_.clear();
  invite_link_.clear();
  controller_->InviteFriend(
      name, base::BindOnce(&SyncBridge::OnInvite, weak_factory_.GetWeakPtr()));
}

void SyncBridge::OnInvite(bool ok, std::string error, std::string link) {
  if (ok) {
    invite_link_ = std::move(link);
  } else {
    people_error_ = std::move(error);
  }
  // The new invite shows in the list.
  LoadPeopleNow();
  OnSyncControllerChanged();
}

void SyncBridge::RemoveFromServer(JNIEnv* env, const std::string& target) {
  if (!controller_) {
    return;
  }
  people_error_.clear();
  controller_->RemoveFromServer(
      target,
      base::BindOnce(&SyncBridge::OnPeopleDone, weak_factory_.GetWeakPtr()));
}

void SyncBridge::OnPeopleDone(bool ok, std::string error) {
  people_error_ = ok ? std::string() : std::move(error);
  LoadPeopleNow();
  OnSyncControllerChanged();
}

void SyncBridge::SetServerPushRelay(JNIEnv* env, bool on) {
  if (!controller_ || people_busy_) {
    return;
  }
  people_error_.clear();
  people_busy_ = true;
  // The switch moves at once; the server's answer settles it.
  if (people_) {
    people_->push_relay = on;
  }
  OnSyncControllerChanged();
  controller_->SetServerPushRelay(
      on,
      base::BindOnce(&SyncBridge::OnPushRelaySet, weak_factory_.GetWeakPtr()));
}

void SyncBridge::OnPushRelaySet(bool ok, std::string error) {
  people_busy_ = false;
  people_error_ = ok ? std::string() : std::move(error);
  // What the server has now (the switch goes back if it failed).
  LoadPeopleNow();
  OnSyncControllerChanged();
}

void SyncBridge::UpdateServer(JNIEnv* env, const std::string& token) {
  if (!controller_) {
    return;
  }
  update_error_.clear();
  // Without a token, the service opens Cloudflare's sign-in first.
  opens_cloudflare_ = token.empty();
  controller_->UpdateServer(token,
                            base::BindOnce(&SyncBridge::OnServerUpdated,
                                           weak_factory_.GetWeakPtr()));
}

void SyncBridge::OnServerUpdated(bool ok, std::string error) {
  update_error_ = ok ? std::string() : std::move(error);
  // Updated: the offer goes (the server reports its new version).
  LoadPeopleNow();
  OnSyncControllerChanged();
}

void SyncBridge::OnSyncControllerChanged() {
  JNIEnv* env = jni_zero::AttachCurrentThread();
  if (controller_) {
    // "Sign in with Cloudflare" from this screen: it steps aside while the
    // tab is open, and comes back when it closes (signed in or not).
    const bool signing_in = controller_->GetStatus().cloudflare_sign_in;
    if (signing_in && opens_cloudflare_ && !cloudflare_open_) {
      cloudflare_open_ = true;
      Java_SyncBridge_cloudflareSignIn(env, java_object_, true);
    } else if (!signing_in && cloudflare_open_) {
      cloudflare_open_ = false;
      opens_cloudflare_ = false;
      Java_SyncBridge_cloudflareSignIn(env, java_object_, false);
    } else if (!signing_in) {
      opens_cloudflare_ = false;
    }
    MaybeLoadPeople();
  }
  // Company sign-in: the screen that started it opens the server's page in
  // a tab, once per page, and closes it when the sign-in ends (the service
  // opens it itself on the desktop).
  if (controller_) {
    const sync::SyncLogin login = controller_->GetStatus().login;
    if (!login.in_progress) {
      if (!opened_login_url_.empty()) {
        Java_SyncBridge_closeLoginPage(env, java_object_);
      }
      opened_login_url_.clear();
      opens_login_ = false;
    } else if (opens_login_ && !login.url.empty() &&
               login.url != opened_login_url_) {
      opened_login_url_ = login.url;
      Java_SyncBridge_openLoginPage(env, java_object_, login.url);
    }
  }
  Java_SyncBridge_onChanged(env, java_object_);
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
