// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/android/scoped_java_ref.h"
#include "base/json/json_writer.h"
#include "base/scoped_observation.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "compass/browser/adblock/adblock_prefs.h"
#include "compass/browser/adblock/adblock_service.h"
#include "compass/strings/grit/compass_strings.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"

// Must come after the includes it uses.
#include "compass/android/jni_headers/AdBlockBridge_jni.h"
#include "ui/base/l10n/l10n_util.h"

namespace compass {

namespace {

adblock::AdBlockService* Service() {
  return adblock::AdBlockService::Get();
}

}  // namespace

// Tells the Java side (the settings' Ad Blocker page) when the lists or the
// switch change. The service is the browser's, so this follows it, not a
// profile.
class AdBlockBridge : public adblock::AdBlockService::Observer {
 public:
  AdBlockBridge(JNIEnv* env, const base::android::JavaRef<jobject>& caller)
      : java_object_(env, caller) {
    if (adblock::AdBlockService* service = Service()) {
      observation_.Observe(service);
    }
  }

  void Destroy(JNIEnv* env) { delete this; }

  // adblock::AdBlockService::Observer:
  void OnFilterListsChanged() override {
    Java_AdBlockBridge_onChanged(jni_zero::AttachCurrentThread(),
                                 java_object_);
  }

 private:
  ~AdBlockBridge() override = default;

  base::android::ScopedJavaGlobalRef<jobject> java_object_;
  base::ScopedObservation<adblock::AdBlockService,
                          adblock::AdBlockService::Observer>
      observation_{this};
};

static int64_t JNI_AdBlockBridge_Init(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& caller) {
  return reinterpret_cast<int64_t>(new AdBlockBridge(env, caller));
}

static bool JNI_AdBlockBridge_IsEnabled(JNIEnv* env) {
  return g_browser_process->local_state()->GetBoolean(
      adblock::prefs::kEnabled);
}

static void JNI_AdBlockBridge_SetEnabled(JNIEnv* env, bool enabled) {
  g_browser_process->local_state()->SetBoolean(adblock::prefs::kEnabled,
                                               enabled);
}

static std::string JNI_AdBlockBridge_ListsJson(JNIEnv* env) {
  base::ListValue lists;
  if (adblock::AdBlockService* service = Service()) {
    for (const adblock::AdBlockService::ListInfo& info : service->GetLists()) {
      lists.Append(
          base::DictValue()
              .Set("id", info.list.id)
              .Set("title", info.list.title)
              .Set("url", info.list.url)
              .Set("builtin", info.list.builtin)
              .Set("enabled", info.list.enabled)
              .Set("urlRules", info.state.url_rules)
              .Set("cssRules", info.state.css_rules)
              .Set("skipped", info.state.skipped)
              .Set("updatedMs",
                   info.state.updated.is_null()
                       ? 0.0
                       : info.state.updated.InMillisecondsFSinceUnixEpoch())
              .Set("error", info.state.error)
              .Set("updating", info.updating));
    }
  }
  return base::WriteJson(lists).value_or("[]");
}

static void JNI_AdBlockBridge_SetListEnabled(JNIEnv* env,
                                             const std::string& id,
                                             bool enabled) {
  if (adblock::AdBlockService* service = Service()) {
    service->SetListEnabled(id, enabled);
  }
}

static std::string JNI_AdBlockBridge_AddList(JNIEnv* env,
                                             const std::string& url) {
  adblock::AdBlockService* service = Service();
  return service ? service->AddList(url)
                 : l10n_util::GetStringUTF8(IDS_COMPASS_ADBLOCK_NOT_RUNNING);
}

static void JNI_AdBlockBridge_RemoveList(JNIEnv* env, const std::string& id) {
  if (adblock::AdBlockService* service = Service()) {
    service->RemoveList(id);
  }
}

static void JNI_AdBlockBridge_UpdateNow(JNIEnv* env) {
  if (adblock::AdBlockService* service = Service()) {
    service->UpdateNow();
  }
}

}  // namespace compass

DEFINE_JNI(AdBlockBridge)
