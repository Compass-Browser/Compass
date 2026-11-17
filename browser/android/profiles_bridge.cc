// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ProfilesBridge.java's natives: a window's profile by its directory's name
// (docs/design/android.md, "Profiles").

#include <string>

#include "base/android/callback_android.h"
#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/android/scoped_java_ref.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"

// Must come after the includes it uses.
#include "compass/android/jni_headers/ProfilesBridge_jni.h"

namespace compass {

static void JNI_ProfilesBridge_LoadProfile(
    JNIEnv* env,
    const std::string& dir,
    const base::android::JavaRef<jobject>& callback) {
  auto done = base::BindOnce(
      [](base::android::ScopedJavaGlobalRef<jobject> callback,
         Profile* profile) {
        base::android::RunObjectCallbackAndroid(
            callback, profile ? profile->GetJavaObject()
                              : base::android::ScopedJavaLocalRef<jobject>());
      },
      base::android::ScopedJavaGlobalRef<jobject>(env, callback));
  ProfileManager* profile_manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  // A name, not a path: nothing outside the user data directory.
  const base::FilePath name = base::FilePath::FromASCII(dir);
  if (!profile_manager || dir.empty() || name.BaseName() != name ||
      name.ReferencesParent()) {
    std::move(done).Run(nullptr);
    return;
  }
  const base::FilePath path = profile_manager->user_data_dir().Append(name);
  // Only a profile that exists: a window never makes one.
  if (!profile_manager->GetProfileAttributesStorage()
           .GetProfileAttributesWithPath(path)) {
    std::move(done).Run(nullptr);
    return;
  }
  // Runs |done| either way (with null if it can't load).
  profile_manager->LoadProfileByPath(path, /*incognito=*/false,
                                     std::move(done));
}

static std::string JNI_ProfilesBridge_GetProfileDir(JNIEnv* env,
                                                    Profile* profile) {
  return profile ? profile->GetPath().BaseName().AsUTF8Unsafe()
                 : std::string();
}

}  // namespace compass

DEFINE_JNI(ProfilesBridge)
