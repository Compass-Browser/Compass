// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_SPACES_BRIDGE_H_
#define COMPASS_BROWSER_ANDROID_SPACES_BRIDGE_H_

#include <jni.h>

#include <string>
#include <vector>

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
  void TabClosureUndone(JNIEnv* env,
                        int32_t tab_id,
                        const std::string& url,
                        const std::u16string& title);

  // The archive (docs/design/android.md, P1).
  void ArchiveNode(JNIEnv* env, const std::string& node_id);
  // Returns the restored entry's tab node ID (empty if none), which the
  // shell opens.
  std::string RestoreArchived(JNIEnv* env, const std::string& entry_id);
  void RemoveArchived(JNIEnv* env, const std::string& entry_id);
  // This window's space's archive and delay.
  void ClearArchive(JNIEnv* env);
  void SetArchiveAfter(JNIEnv* env, int32_t hours);

  // Pinned URL (docs/design/android.md, P2): the pinned tab's or favorite's
  // page becomes its pinned URL; and its pinned URL, where "Back to Pinned
  // URL" goes (an unloaded entry's page is set to it here).
  void UseCurrentPageAsSavedUrl(JNIEnv* env, const std::string& node_id);
  std::string BackToSavedUrl(JNIEnv* env, const std::string& node_id);

  // SpacesAndroid::Observer:
  void OnSpacesChanged() override;
  void OnTabsArchived(const std::vector<int>& tab_ids) override;

 private:
  base::android::ScopedJavaGlobalRef<jobject> java_object_;
  raw_ptr<SpacesAndroid> spaces_;
  // The space this window shows (SpacesAndroid::SpaceOrActive()): set when
  // the window switches spaces, and taken from the list's active space until
  // then, or once its space is gone.
  base::Uuid space_id_;
  base::ScopedObservation<SpacesAndroid, SpacesAndroid::Observer>
      observation_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_SPACES_BRIDGE_H_
