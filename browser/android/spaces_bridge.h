// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_SPACES_BRIDGE_H_
#define COMPASS_BROWSER_ANDROID_SPACES_BRIDGE_H_

#include <jni.h>

#include <string>

#include "base/android/scoped_java_ref.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
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
  // The space this window shows, and showing one without making it the
  // list's active space (the start page follows its window's).
  std::string ShownSpace(JNIEnv* env);
  void ShowSpace(JNIEnv* env, const std::string& space_id);
  void SetActiveSpace(JNIEnv* env, const std::string& space_id);
  std::string AddSpace(JNIEnv* env, const std::u16string& name);
  void EditSpace(JNIEnv* env,
                 const std::string& space_id,
                 const std::u16string& name,
                 const std::string& emoji,
                 const std::string& tint);
  void RemoveSpace(JNIEnv* env, const std::string& space_id);
  void Pin(JNIEnv* env, int32_t tab_id, bool favorite);
  // |section|: "pinned", "today" or "favorites" (SpacesAndroid::MoveNode()).
  void MoveNode(JNIEnv* env,
                const std::string& node_id,
                const std::string& space_id,
                const std::string& section);
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

  // Live folders (docs/design/live-folders.md), through SpacesAndroid's
  // LiveFolderService; JSON goes both ways (SpacesBridge.java says what's
  // in it). Errors are messages for the user, empty for none.
  // The plugins a folder can use, to |callback| (a Callback<String>).
  void LivePlugins(JNIEnv* env,
                   const base::android::JavaParamRef<jobject>& callback);
  // A live folder's source, for Edit Source; empty if it isn't one.
  std::string LiveFolderSource(JNIEnv* env, const std::string& folder_id);
  // Makes a live folder at the end of this window's space's pinned tree
  // (|folder_id| empty), or changes |folder_id|'s source.
  std::string SaveLiveFolder(JNIEnv* env,
                             const std::string& folder_id,
                             const std::string& plugin_id,
                             const std::u16string& title,
                             const std::string& config_json,
                             const std::string& secrets_json,
                             bool grant_cookies);
  void RefreshLiveFolder(JNIEnv* env, const std::string& folder_id);
  void MarkAllSeen(JNIEnv* env, const std::string& folder_id);
  // Its entries become ordinary pinned tabs.
  void ConvertLiveFolder(JNIEnv* env, const std::string& folder_id);
  // Installing a plugin from its manifest's address: what it is, for review
  // ({"preview": …} or {"error": …}), then installing it if it's still that
  // (an error, or empty).
  void PreviewLivePlugin(JNIEnv* env,
                         const std::string& url,
                         const base::android::JavaParamRef<jobject>& callback);
  void InstallLivePlugin(JNIEnv* env,
                         const std::string& url,
                         const std::string& sha256,
                         const base::android::JavaParamRef<jobject>& callback);
  // Removes an installed plugin and its secrets; |callback| gets "".
  void RemoveLivePlugin(JNIEnv* env,
                        const std::string& plugin_id,
                        const base::android::JavaParamRef<jobject>& callback);

  // SpacesAndroid::Observer:
  void OnSpacesChanged() override;

 private:
  base::android::ScopedJavaGlobalRef<jobject> java_object_;
  raw_ptr<SpacesAndroid> spaces_;
  // The space this window shows (SpacesAndroid::SpaceOrActive()): set when
  // the window switches spaces, and taken from the list's active space until
  // then, or once its space is gone.
  base::Uuid space_id_;
  base::ScopedObservation<SpacesAndroid, SpacesAndroid::Observer>
      observation_{this};
  base::WeakPtrFactory<SpacesBridge> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_SPACES_BRIDGE_H_
