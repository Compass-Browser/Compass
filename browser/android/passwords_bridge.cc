// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/passwords_bridge.h"

#include <utility>

#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"

// Must come after the class declaration.
#include "compass/android/jni_headers/PasswordsBridge_jni.h"

namespace compass {

PasswordsBridge::PasswordsBridge(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& java_object,
    Profile* profile)
    : java_object_(env, java_object), profile_(profile) {}

PasswordsBridge::~PasswordsBridge() = default;

void PasswordsBridge::Destroy(JNIEnv* env) {
  delete this;
}

void PasswordsBridge::Load(JNIEnv* env) {
  scoped_refptr<password_manager::PasswordStoreInterface> store =
      ProfilePasswordStoreFactory::GetForProfile(
          profile_, ServiceAccessType::EXPLICIT_ACCESS);
  if (!store) {
    Java_PasswordsBridge_onLoaded(env, java_object_, std::string("[]"));
    return;
  }
  store->GetAutofillableLogins(weak_factory_.GetWeakPtr());
}

std::u16string PasswordsBridge::PasswordAt(JNIEnv* env, int32_t index) {
  if (index < 0 || static_cast<size_t>(index) >= credentials_.size()) {
    return std::u16string();
  }
  return credentials_[index].password_value.value();
}

void PasswordsBridge::OnGetPasswordStoreResultsOrErrorFrom(
    password_manager::PasswordStoreInterface* store,
    base::expected<std::vector<password_manager::StoredCredential>,
                   password_manager::PasswordStoreBackendError>
        results_or_error) {
  credentials_.clear();
  base::ListValue list;
  if (results_or_error.has_value()) {
    for (password_manager::StoredCredential& credential :
         *results_or_error) {
      // Federated sign-ins have no password to fill.
      if (credential.password_value.empty()) {
        continue;
      }
      list.Append(base::DictValue()
                      .Set("site", credential.signon_realm)
                      .Set("url", credential.url.spec())
                      .Set("username",
                           base::UTF16ToUTF8(credential.username_value)));
      credentials_.push_back(std::move(credential));
    }
  }
  Java_PasswordsBridge_onLoaded(jni_zero::AttachCurrentThread(), java_object_,
                                base::WriteJson(list).value_or("[]"));
}

static int64_t JNI_PasswordsBridge_Init(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& caller,
    Profile* profile) {
  return reinterpret_cast<int64_t>(new PasswordsBridge(env, caller, profile));
}

}  // namespace compass

DEFINE_JNI(PasswordsBridge)
