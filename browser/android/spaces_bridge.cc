// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/spaces_bridge.h"

#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "url/gurl.h"

// Must come after the class declaration.
#include "compass/android/jni_headers/SpacesBridge_jni.h"

namespace compass {

SpacesBridge::SpacesBridge(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& java_object,
    Profile* profile)
    : java_object_(env, java_object), spaces_(SpacesAndroid::Get(profile)) {
  observation_.Observe(spaces_.get());
}

SpacesBridge::~SpacesBridge() = default;

void SpacesBridge::Destroy(JNIEnv* env) {
  delete this;
}

std::string SpacesBridge::Snapshot(JNIEnv* env) {
  return spaces_->Snapshot();
}

void SpacesBridge::SetActiveSpace(JNIEnv* env, const std::string& space_id) {
  if (SpaceModel* space =
          spaces_->list().FindSpace(base::Uuid::ParseLowercase(space_id))) {
    spaces_->list().SetActiveSpace(*space);
  }
}

std::string SpacesBridge::AddSpace(JNIEnv* env, const std::u16string& name) {
  SpaceList& list = spaces_->list();
  SpaceModel* space = list.AddSpace(list.spaces().size(), name);
  if (!space) {
    return std::string();
  }
  list.SetActiveSpace(*space);
  return space->id().AsLowercaseString();
}

void SpacesBridge::Pin(JNIEnv* env, int32_t tab_id, bool favorite) {
  spaces_->Pin(tab_id, favorite);
}

void SpacesBridge::RemoveNode(JNIEnv* env, const std::string& node_id) {
  SidebarNode* node = spaces_->FindNode(base::Uuid::ParseLowercase(node_id));
  if (!node) {
    return;
  }
  SpaceList& list = spaces_->list();
  SpaceModel* space = list.favorites().Contains(node) ? &list.favorites()
                                                       : list.FindSpaceOf(node);
  if (space) {
    space->Remove(node);
  }
}

void SpacesBridge::SetCollapsed(JNIEnv* env,
                                const std::string& folder_id,
                                bool collapsed) {
  SidebarNode* node =
      spaces_->FindNode(base::Uuid::ParseLowercase(folder_id));
  if (!node || node->type() != SidebarNode::Type::kFolder) {
    return;
  }
  if (SpaceModel* space = spaces_->list().FindSpaceOf(node)) {
    space->SetCollapsed(node, collapsed);
  }
}

int32_t SpacesBridge::TabFor(JNIEnv* env, const std::string& node_id) {
  return spaces_->TabFor(base::Uuid::ParseLowercase(node_id));
}

void SpacesBridge::TabOpened(JNIEnv* env,
                             int32_t tab_id,
                             const std::string& url,
                             const std::u16string& title,
                             bool restored) {
  spaces_->TabOpened(tab_id, GURL(url), title, restored);
}

void SpacesBridge::TabOpenedForNode(JNIEnv* env,
                                    int32_t tab_id,
                                    const std::string& node_id) {
  spaces_->TabOpenedForNode(tab_id, base::Uuid::ParseLowercase(node_id));
}

void SpacesBridge::TabUpdated(JNIEnv* env,
                              int32_t tab_id,
                              const std::string& url,
                              const std::u16string& title) {
  spaces_->TabUpdated(tab_id, GURL(url), title);
}

void SpacesBridge::TabSelected(JNIEnv* env, int32_t tab_id) {
  spaces_->TabSelected(tab_id);
}

void SpacesBridge::TabClosed(JNIEnv* env, int32_t tab_id) {
  spaces_->TabClosed(tab_id);
}

void SpacesBridge::OnSpacesChanged() {
  Java_SpacesBridge_onChanged(jni_zero::AttachCurrentThread(),
                              java_object_);
}

static int64_t JNI_SpacesBridge_Init(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& caller,
    Profile* profile) {
  return reinterpret_cast<int64_t>(new SpacesBridge(env, caller, profile));
}

}  // namespace compass

DEFINE_JNI(SpacesBridge)
