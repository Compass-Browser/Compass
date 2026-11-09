// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_SPACES_BRIDGE_H_
#define COMPASS_BROWSER_ANDROID_SPACES_BRIDGE_H_

#include <jni.h>

#include <string>

#include "base/android/scoped_java_ref.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "compass/browser/android/spaces_android.h"

class Profile;

namespace compass {

// The native half of it.cipi1965.compass.spaces.SpacesBridge: one per Java
// object, for one profile's SpacesAndroid. Owned by the Java object
// (destroy()).
class SpacesBridge : public SpacesAndroid::Observer {
 public:
  SpacesBridge(JNIEnv* env,
               const base::android::JavaRef<jobject>& java_object,
               Profile* profile);
  SpacesBridge(const SpacesBridge&) = delete;
  SpacesBridge& operator=(const SpacesBridge&) = delete;
  ~SpacesBridge() override;

  void Destroy(JNIEnv* env);
  std::string Snapshot(JNIEnv* env);
  void SetActiveSpace(JNIEnv* env, const std::string& space_id);
  std::string AddSpace(JNIEnv* env, const std::u16string& name);
  void EditSpace(JNIEnv* env,
                 const std::string& space_id,
                 const std::u16string& name,
                 const std::string& emoji,
                 const std::string& tint);
  void RemoveSpace(JNIEnv* env, const std::string& space_id);
  void Pin(JNIEnv* env, int32_t tab_id, bool favorite);
  void RemoveNode(JNIEnv* env, const std::string& node_id);
  void SetCollapsed(JNIEnv* env, const std::string& folder_id, bool collapsed);
  int32_t TabFor(JNIEnv* env, const std::string& node_id);
  void TabOpened(JNIEnv* env,
                 int32_t tab_id,
                 const std::string& url,
                 const std::u16string& title,
                 bool restored);
  void TabOpenedForNode(JNIEnv* env,
                        int32_t tab_id,
                        const std::string& node_id);
  void TabUpdated(JNIEnv* env,
                  int32_t tab_id,
                  const std::string& url,
                  const std::u16string& title);
  void TabSelected(JNIEnv* env, int32_t tab_id);
  void TabClosed(JNIEnv* env, int32_t tab_id);

  // SpacesAndroid::Observer:
  void OnSpacesChanged() override;

 private:
  base::android::ScopedJavaGlobalRef<jobject> java_object_;
  raw_ptr<SpacesAndroid> spaces_;
  base::ScopedObservation<SpacesAndroid, SpacesAndroid::Observer>
      observation_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_SPACES_BRIDGE_H_
