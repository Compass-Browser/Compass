// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// EaselBridge.java's natives (docs/design/android.md, "Easels"): New Easel
// and Capture to Easel on Android. A capture is the whole visible page; the
// board's editor crops it.

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "base/android/callback_android.h"
#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/android/spaces_android.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/easels/easel_capture_core.h"
#include "compass/browser/ui/easels/easel_titles.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

// Must come after the includes it uses.
#include "compass/android/jni_headers/EaselBridge_jni.h"

using base::android::JavaRef;
using base::android::ScopedJavaGlobalRef;

namespace compass {

namespace {

std::string Json(const base::DictValue& dict) {
  std::string json;
  base::JSONWriter::Write(dict, &json);
  return json;
}

std::string Error(std::string_view message) {
  return Json(base::DictValue().Set("error", message));
}

// A tab entry for |easel| at the end of |space_id|'s Pinned (Today's top
// without one), as the desktop's PinEaselTab(); null when the spaces
// aren't loaded.
SidebarNode* PinEaselTab(Profile* profile,
                         const std::string& space_id,
                         const sync_pb::CompassEasel& easel) {
  SpacesAndroid* spaces = SpacesAndroid::Get(profile);
  if (!spaces || !spaces->loaded()) {
    return nullptr;
  }
  SpaceModel& space =
      spaces->SpaceOrActive(base::Uuid::ParseLowercase(space_id));
  SidebarNode* parent =
      space.has_pinned() ? space.pinned_root() : space.today_root();
  if (!space.CanContain(parent, SidebarNode::Type::kTab)) {
    return nullptr;
  }
  const size_t index = space.has_pinned() ? parent->children().size() : 0;
  return space.AddTab(parent, index, GURL(easels::EaselUrl(easel.uuid())),
                      easels::EaselTabTitle(easel));
}

}  // namespace

// The boards as the picker lists them: [{id, title}], the last used first.
static std::string JNI_EaselBridge_Easels(JNIEnv* env, Profile* profile) {
  base::ListValue list;
  if (profile) {
    for (const sync_pb::CompassEasel* easel :
         easels::EaselsForPicker(profile)) {
      list.Append(base::DictValue()
                      .Set("id", easel->uuid())
                      .Set("title", easels::EaselTitleOf(*easel)));
    }
  }
  std::string json;
  base::JSONWriter::Write(list, &json);
  return json;
}

// New Easel: {nodeId, url} of its pinned tab entry, for the shell to open,
// or {error}.
static std::string JNI_EaselBridge_NewEasel(JNIEnv* env,
                                            Profile* profile,
                                            const std::string& space_id) {
  easels::EaselStore* store =
      profile ? easels::EaselStore::Get(profile) : nullptr;
  if (!store) {
    return Error("Easels aren't available here.");
  }
  base::expected<sync_pb::CompassEasel, std::string> easel =
      store->CreateEasel(easels::kNewEaselTitle);
  if (!easel.has_value()) {
    return Error(easel.error());
  }
  SidebarNode* node = PinEaselTab(profile, space_id, *easel);
  if (!node) {
    return Error("The sidebar is still loading. Try again in a moment.");
  }
  return Json(base::DictValue()
                  .Set("nodeId", node->id().AsLowercaseString())
                  .Set("url", node->url().spec()));
}

// Capture to Easel: the visible page to |easel_uuid| (empty: a new board,
// pinned in |space_id|). The callback gets {title} of the board, or
// {error}.
static void JNI_EaselBridge_Capture(JNIEnv* env,
                                    content::WebContents* contents,
                                    const std::string& easel_uuid,
                                    const std::string& space_id,
                                    const JavaRef<jobject>& j_callback) {
  ScopedJavaGlobalRef<jobject> callback(j_callback);
  if (!contents) {
    base::android::RunStringCallbackAndroid(callback,
                                            Error("There's no page."));
    return;
  }
  easels::RunPageScript(
      contents, "__compassEasel.probe()",
      base::BindOnce(
          [](base::WeakPtr<content::WebContents> contents,
             std::string easel_uuid, std::string space_id,
             ScopedJavaGlobalRef<jobject> callback, base::Value value) {
            std::optional<easels::PageView> view =
                easels::PageViewFromValue(value);
            if (!contents || !view) {
              base::android::RunStringCallbackAndroid(
                  callback, Error("This page can't be captured."));
              return;
            }
            easels::CaptureRequest request;
            request.easel_uuid = easel_uuid;
            // The layout viewport as it shows (pinch zoom aside: the
            // visible part of it).
            request.region = easels::ToRectF(
                view->visual_left, view->visual_top,
                view->viewport_width / std::max(view->visual_scale, 1e-3),
                view->viewport_height / std::max(view->visual_scale, 1e-3));
            request.view = *view;
            Profile* profile =
                Profile::FromBrowserContext(contents->GetBrowserContext());
            easels::AddCaptureToEasel(
                contents.get(), std::move(request),
                base::BindOnce(
                    [](base::WeakPtr<Profile> profile, bool new_easel,
                       std::string space_id,
                       ScopedJavaGlobalRef<jobject> callback,
                       base::expected<sync_pb::CompassEaselObject,
                                      std::string> saved) {
                      if (!saved.has_value()) {
                        base::android::RunStringCallbackAndroid(
                            callback, Error(saved.error()));
                        return;
                      }
                      easels::EaselStore* store =
                          profile ? easels::EaselStore::Get(profile.get())
                                  : nullptr;
                      const sync_pb::CompassEasel* easel =
                          store ? store->FindEasel(saved->easel_uuid())
                                : nullptr;
                      if (!easel) {
                        base::android::RunStringCallbackAndroid(
                            callback, Error("That Easel was deleted."));
                        return;
                      }
                      if (new_easel) {
                        PinEaselTab(profile.get(), space_id, *easel);
                      }
                      base::android::RunStringCallbackAndroid(
                          callback,
                          Json(base::DictValue().Set(
                              "title", easels::EaselTitleOf(*easel))));
                    },
                    profile->GetWeakPtr(), easel_uuid.empty(), space_id,
                    callback));
          },
          contents->GetWeakPtr(), easel_uuid, space_id, callback));
}

}  // namespace compass

DEFINE_JNI(EaselBridge)
