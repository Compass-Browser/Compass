// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ProfilesBridge.java's natives: a window's profile by its directory's name
// (docs/design/android.md, "Profiles").

#include <string>

#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/android/scoped_java_ref.h"
#include "base/files/file_path.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"

// Must come after the includes it uses.
#include "compass/android/jni_headers/ProfilesBridge_jni.h"

namespace compass {

static base::android::ScopedJavaLocalRef<jobject>
JNI_ProfilesBridge_LoadProfileSync(JNIEnv* env, const std::string& dir) {
  ProfileManager* profile_manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  const base::FilePath name = base::FilePath::FromASCII(dir);
  if (!profile_manager || dir.empty() || name.BaseName() != name ||
      name.ReferencesParent()) {
    // A name, not a path: nothing outside the user data directory.
    return nullptr;
  }
  const base::FilePath path = profile_manager->user_data_dir().Append(name);
  // Only a profile that exists: a window never makes one.
  if (!profile_manager->GetProfileAttributesStorage()
           .GetProfileAttributesWithPath(path)) {
    return nullptr;
  }
  // Loads it now if it isn't loaded: a window made for this profile needs it
  // at once, as Chrome's startup profile (ProfileManager::GetProfile()).
  Profile* profile = profile_manager->GetProfileByPath(path);
  if (!profile) {
    profile = profile_manager->GetProfile(path);
  }
  return profile ? profile->GetJavaObject() : nullptr;
}

static std::string JNI_ProfilesBridge_GetProfileDir(JNIEnv* env,
                                                    Profile* profile) {
  return profile ? profile->GetPath().BaseName().AsUTF8Unsafe()
                 : std::string();
}

}  // namespace compass

DEFINE_JNI(ProfilesBridge)
