// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/spaces_bridge.h"

#include "base/strings/string_number_conversions.h"
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

SpacesBridge::~SpacesBridge() {
  spaces_->WindowClosed(this);
}

void SpacesBridge::Destroy(JNIEnv* env) {
  delete this;
}

std::string SpacesBridge::Snapshot(JNIEnv* env) {
  if (spaces_->loaded()) {
    // Adopts the list's active space until this window picks one, and again
    // once its space is gone (removed here, or by sync).
    space_id_ = spaces_->SpaceOrActive(space_id_).id();
  }
  return spaces_->Snapshot(space_id_);
}

std::string SpacesBridge::ShownSpace(JNIEnv* env) {
  return spaces_->loaded() ? spaces_->SpaceOrActive(space_id_)
                                 .id()
                                 .AsLowercaseString()
                           : std::string();
}

void SpacesBridge::ShowSpace(JNIEnv* env, const std::string& space_id) {
  base::Uuid id = base::Uuid::ParseLowercase(space_id);
  if (id.is_valid()) {
    space_id_ = id;
  }
}

void SpacesBridge::SetActiveSpace(JNIEnv* env, const std::string& space_id) {
  if (SpaceModel* space =
          spaces_->list().FindSpace(base::Uuid::ParseLowercase(space_id))) {
    space_id_ = space->id();
    // The list's active space is the last one a window showed.
    spaces_->list().SetActiveSpace(*space);
  }
}

std::string SpacesBridge::AddSpace(JNIEnv* env, const std::u16string& name) {
  SpaceList& list = spaces_->list();
  SpaceModel* space = list.AddSpace(list.spaces().size(), name);
  if (!space) {
    return std::string();
  }
  space_id_ = space->id();
  list.SetActiveSpace(*space);
  return space->id().AsLowercaseString();
}

void SpacesBridge::EditSpace(JNIEnv* env,
                             const std::string& space_id,
                             const std::u16string& name,
                             const std::string& emoji,
                             const std::string& tint) {
  SpaceModel* space =
      spaces_->list().FindSpace(base::Uuid::ParseLowercase(space_id));
  if (!space) {
    return;
  }
  if (!name.empty() && name != space->name()) {
    space->SetName(name);
  }
  std::optional<SpaceIcon> icon =
      emoji.empty() ? SpaceIcon() : SpaceIcon::Emoji(emoji);
  if (icon && *icon != space->icon()) {
    space->SetIcon(*icon);
  }
  SpaceColor color;
  uint32_t rgb = 0;
  if (tint.size() == 7 && tint[0] == '#' &&
      base::HexStringToUInt(std::string_view(tint).substr(1), &rgb)) {
    color = SpaceColor::FromRGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff,
                                rgb & 0xff);
  }
  if (color != space->color()) {
    space->SetColor(color);
  }
}

void SpacesBridge::RemoveSpace(JNIEnv* env, const std::string& space_id) {
  SpaceList& list = spaces_->list();
  SpaceModel* space = list.FindSpace(base::Uuid::ParseLowercase(space_id));
  if (space && list.spaces().size() > 1) {
    list.RemoveSpace(*space);
  }
}

void SpacesBridge::Pin(JNIEnv* env, int32_t tab_id, bool favorite) {
  spaces_->Pin(tab_id, favorite);
}

void SpacesBridge::MoveNode(JNIEnv* env,
                            const std::string& node_id,
                            const std::string& space_id,
                            const std::string& section) {
  SpacesAndroid::Section where = section == "favorites"
                                     ? SpacesAndroid::Section::kFavorites
                                 : section == "today"
                                     ? SpacesAndroid::Section::kToday
                                     : SpacesAndroid::Section::kPinned;
  // Within this window's space unless another is named.
  base::Uuid space = space_id.empty() ? spaces_->SpaceOrActive(space_id_).id()
                                      : base::Uuid::ParseLowercase(space_id);
  spaces_->MoveNode(base::Uuid::ParseLowercase(node_id), space, where);
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
  spaces_->TabOpened(tab_id, GURL(url), title, restored, space_id_);
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
  spaces_->TabSelected(tab_id, this);
}

void SpacesBridge::TabClosed(JNIEnv* env, int32_t tab_id) {
  spaces_->TabClosed(tab_id);
}

void SpacesBridge::TabClosureUndone(JNIEnv* env,
                                    int32_t tab_id,
                                    const std::string& url,
                                    const std::u16string& title) {
  spaces_->TabClosureUndone(tab_id, GURL(url), title, space_id_);
}

void SpacesBridge::ArchiveNode(JNIEnv* env, const std::string& node_id) {
  spaces_->ArchiveNode(base::Uuid::ParseLowercase(node_id));
}

std::string SpacesBridge::RestoreArchived(JNIEnv* env,
                                          const std::string& entry_id) {
  SidebarNode* node =
      spaces_->RestoreArchived(base::Uuid::ParseLowercase(entry_id));
  return node ? node->id().AsLowercaseString() : std::string();
}

void SpacesBridge::RemoveArchived(JNIEnv* env, const std::string& entry_id) {
  spaces_->RemoveArchived(base::Uuid::ParseLowercase(entry_id));
}

void SpacesBridge::ClearArchive(JNIEnv* env) {
  if (spaces_->loaded()) {
    spaces_->SpaceOrActive(space_id_).ClearArchive();
  }
}

void SpacesBridge::SetArchiveAfter(JNIEnv* env, int32_t hours) {
  if (spaces_->loaded()) {
    spaces_->SetArchiveAfter(spaces_->SpaceOrActive(space_id_).id(), hours);
  }
}

void SpacesBridge::UseCurrentPageAsSavedUrl(JNIEnv* env,
                                            const std::string& node_id) {
  spaces_->UseCurrentPageAsSavedUrl(base::Uuid::ParseLowercase(node_id));
}

std::string SpacesBridge::BackToSavedUrl(JNIEnv* env,
                                         const std::string& node_id) {
  SidebarNode* node = spaces_->FindNode(base::Uuid::ParseLowercase(node_id));
  if (!node || !node->saved_url().is_valid()) {
    return std::string();
  }
  // With no tab, the entry itself goes back; an open tab is loaded there by
  // the shell, and its entry follows.
  if (spaces_->TabFor(node->id()) == -1 && node->url() != node->saved_url()) {
    SpaceList& list = spaces_->list();
    SpaceModel* space = list.favorites().Contains(node)
                            ? &list.favorites()
                            : list.FindSpaceOf(node);
    if (space) {
      space->SetUrl(node, node->saved_url());
      if (!node->saved_title().empty()) {
        space->SetTitle(node, node->saved_title());
      }
    }
  }
  return node->saved_url().spec();
}

void SpacesBridge::OnSpacesChanged() {
  Java_SpacesBridge_onChanged(jni_zero::AttachCurrentThread(),
                              java_object_);
}

void SpacesBridge::OnTabsArchived(const std::vector<int>& tab_ids) {
  JNIEnv* env = jni_zero::AttachCurrentThread();
  for (int tab_id : tab_ids) {
    Java_SpacesBridge_onTabArchived(env, java_object_, tab_id);
  }
}

static int64_t JNI_SpacesBridge_Init(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& caller,
    Profile* profile) {
  return reinterpret_cast<int64_t>(new SpacesBridge(env, caller, profile));
}

}  // namespace compass

DEFINE_JNI(SpacesBridge)
