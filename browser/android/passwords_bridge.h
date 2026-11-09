// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_PASSWORDS_BRIDGE_H_
#define COMPASS_BROWSER_ANDROID_PASSWORDS_BRIDGE_H_

#include <jni.h>

#include <string>
#include <vector>

#include "base/android/scoped_java_ref.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/password_manager/core/browser/password_store/password_store_consumer.h"

class Profile;

namespace compass {

// The native half of it.cipi1965.compass.autofill.PasswordsBridge: the
// profile's saved passwords for the autofill picker (docs/design/android.md,
// "Passwords in other apps"). Java gets each one's site and username; a
// password crosses only for the one the user picks.
class PasswordsBridge : public password_manager::PasswordStoreConsumer {
 public:
  PasswordsBridge(JNIEnv* env,
                  const base::android::JavaRef<jobject>& java_object,
                  Profile* profile);
  PasswordsBridge(const PasswordsBridge&) = delete;
  PasswordsBridge& operator=(const PasswordsBridge&) = delete;
  ~PasswordsBridge() override;

  void Destroy(JNIEnv* env);
  // Reads the saved passwords; onLoaded(json) follows, with a list of
  // {"site", "url", "username"} in the order of PasswordAt().
  void Load(JNIEnv* env);
  std::u16string PasswordAt(JNIEnv* env, int32_t index);

  // password_manager::PasswordStoreConsumer:
  void OnGetPasswordStoreResultsOrErrorFrom(
      password_manager::PasswordStoreInterface* store,
      base::expected<std::vector<password_manager::StoredCredential>,
                     password_manager::PasswordStoreBackendError>
          results_or_error) override;

 private:
  base::android::ScopedJavaGlobalRef<jobject> java_object_;
  raw_ptr<Profile> profile_;
  std::vector<password_manager::StoredCredential> credentials_;
  base::WeakPtrFactory<PasswordsBridge> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_PASSWORDS_BRIDGE_H_
