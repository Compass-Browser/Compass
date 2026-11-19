// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/claude_sidebar_bridge.h"

#include <utility>

#include "base/android/jni_string.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/android/spaces_android.h"
#include "compass/browser/claude/ai_provider.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/claude/short_titler.h"
#include "components/prefs/pref_service.h"

// Must come after the class declaration.
#include "compass/android/jni_headers/ClaudeSidebarBridge_jni.h"

namespace compass {

ClaudeSidebarBridge::ClaudeSidebarBridge(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& java_object,
    Profile* profile)
    : java_object_(env, java_object),
      profile_(profile),
      quick_(claude::ClaudeQuick::Get(profile)) {
  // The services follow the profile's spaces once made; short titles need
  // nothing more (the sidebar's snapshot shows them).
  ShortTitler::Get(profile);
  if (LinkFolderNamer* namer = LinkFolderNamer::Get(profile)) {
    namer_observation_.Observe(namer);
  }
  if (TodayTidier* tidier = TodayTidier::Get(profile)) {
    tidier_observation_.Observe(tidier);
  }
  // What's offered follows the switches and the key.
  prefs_.Init(profile->GetPrefs());
  for (const char* pref :
       {claude::prefs::kNameLinkFolders, claude::prefs::kTidyToday,
        claude::prefs::kCommandBarAsk}) {
    prefs_.Add(pref, base::BindRepeating(&ClaudeSidebarBridge::Changed,
                                         base::Unretained(this)));
  }
  claude::prefs::ObserveProviderPrefs(
      prefs_, {claude::prefs::kApiKey, claude::prefs::kBaseUrl},
      base::BindRepeating(&ClaudeSidebarBridge::Changed,
                          base::Unretained(this)));
}

ClaudeSidebarBridge::~ClaudeSidebarBridge() {
  if (ask_id_ && quick_) {
    quick_->Cancel(ask_id_);
  }
}

void ClaudeSidebarBridge::Destroy(JNIEnv* env) {
  delete this;
}

std::string ClaudeSidebarBridge::Snapshot(JNIEnv* env) {
  base::DictValue snapshot;
  snapshot.Set("suggest_names",
               quick_ && quick_->IsEnabled(claude::prefs::kNameLinkFolders));
  snapshot.Set("ask", quick_ && quick_->IsEnabled(claude::prefs::kCommandBarAsk));
  TodayTidier* tidier = tidier_observation_.GetSource();
  snapshot.Set("tidy", tidier && tidier->IsEnabled());
  base::ListValue busy;
  if (tidier) {
    for (const auto& space :
         SpacesAndroid::Get(profile_)->space_list().spaces()) {
      if (tidier->IsBusy(space->id())) {
        busy.Append(space->id().AsLowercaseString());
      }
    }
  }
  snapshot.Set("busy", std::move(busy));
  return base::WriteJson(snapshot).value_or("{}");
}

void ClaudeSidebarBridge::SuggestName(JNIEnv* env,
                                      const std::string& folder_id) {
  if (LinkFolderNamer* namer = namer_observation_.GetSource()) {
    namer->Suggest(base::Uuid::ParseLowercase(folder_id));
  }
}

void ClaudeSidebarBridge::Tidy(JNIEnv* env, const std::string& space_id) {
  TodayTidier* tidier = tidier_observation_.GetSource();
  if (!tidier) {
    return;
  }
  SpaceModel* space = SpacesAndroid::Get(profile_)->space_list().FindSpace(
      base::Uuid::ParseLowercase(space_id));
  if (space && tidier->CanTidy(*space)) {
    tidier->Tidy(*space);
  }
}

bool ClaudeSidebarBridge::UndoTidy(JNIEnv* env, int32_t undo_id) {
  TodayTidier* tidier = tidier_observation_.GetSource();
  return tidier && tidier->Undo(undo_id);
}

bool ClaudeSidebarBridge::LooksLikeQuestion(JNIEnv* env,
                                            const std::u16string& text) {
  return claude::LooksLikeQuestion(text,
                                   g_browser_process->GetApplicationLocale());
}

std::string ClaudeSidebarBridge::Ask(JNIEnv* env,
                                     const std::string& question,
                                     const std::string& page_title,
                                     const std::string& page_host) {
  CancelAsk(env);
  if (!quick_ || !quick_->IsEnabled(claude::prefs::kCommandBarAsk) ||
      question.empty()) {
    return std::string();
  }
  claude::QuickRequest request = claude::MakeAskRequest(
      question, claude::DescribeAskPage(page_title, page_host),
      profile_->GetPrefs()->GetString(claude::prefs::CurrentPref(
          *profile_->GetPrefs(), claude::prefs::kCommandBarAskModel)),
      g_browser_process->GetApplicationLocale());
  std::string sent = request.user;
  // The ids start at 1: the callbacks learn theirs through |ask_id_|.
  ask_id_ = quick_->Stream(
      std::move(request),
      base::BindRepeating(
          [](base::WeakPtr<ClaudeSidebarBridge> self, const std::string& text) {
            if (self) {
              self->OnAnswerText(self->ask_id_, text);
            }
          },
          weak_factory_.GetWeakPtr()),
      base::BindOnce(
          [](base::WeakPtr<ClaudeSidebarBridge> self,
             claude::ClaudeQuick::Result result) {
            if (self) {
              self->OnAnswerDone(self->ask_id_, std::move(result));
            }
          },
          weak_factory_.GetWeakPtr()));
  return sent;
}

void ClaudeSidebarBridge::CancelAsk(JNIEnv* env) {
  if (ask_id_ && quick_) {
    quick_->Cancel(ask_id_);
  }
  ask_id_ = 0;
}

void ClaudeSidebarBridge::OnAnswerText(int id, const std::string& text) {
  if (!id || id != ask_id_) {
    return;
  }
  JNIEnv* env = jni_zero::AttachCurrentThread();
  Java_ClaudeSidebarBridge_onAnswer(env, java_object_, text);
}

void ClaudeSidebarBridge::OnAnswerDone(int id,
                                       claude::ClaudeQuick::Result result) {
  if (!id || id != ask_id_) {
    return;
  }
  ask_id_ = 0;
  JNIEnv* env = jni_zero::AttachCurrentThread();
  std::string text;
  std::string error;
  if (result.has_value()) {
    if (const std::string* all = result->FindString("text")) {
      text = *all;
    }
  } else {
    error = claude::DescribeQuickError(result.error());
  }
  Java_ClaudeSidebarBridge_onAnswerDone(env, java_object_, text, error);
}

void ClaudeSidebarBridge::OnFolderNamed(const base::Uuid& folder_id) {
  JNIEnv* env = jni_zero::AttachCurrentThread();
  Java_ClaudeSidebarBridge_onFolderNamed(env, java_object_,
                                         folder_id.AsLowercaseString());
}

void ClaudeSidebarBridge::OnTidyBusyChanged(const base::Uuid& space_id) {
  Changed();
}

void ClaudeSidebarBridge::OnTidied(const base::Uuid& space_id,
                                   int undo_id,
                                   size_t folders) {
  JNIEnv* env = jni_zero::AttachCurrentThread();
  Java_ClaudeSidebarBridge_onTidied(env, java_object_,
                                    space_id.AsLowercaseString(), undo_id,
                                    static_cast<int>(folders));
}

void ClaudeSidebarBridge::OnTidyFailed(const base::Uuid& space_id,
                                       const std::string& message) {
  JNIEnv* env = jni_zero::AttachCurrentThread();
  Java_ClaudeSidebarBridge_onTidyFailed(env, java_object_,
                                        space_id.AsLowercaseString(), message);
}

void ClaudeSidebarBridge::Changed() {
  JNIEnv* env = jni_zero::AttachCurrentThread();
  Java_ClaudeSidebarBridge_onChanged(env, java_object_);
}

static int64_t JNI_ClaudeSidebarBridge_Init(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& caller,
    Profile* profile) {
  return reinterpret_cast<int64_t>(
      new ClaudeSidebarBridge(env, caller, profile));
}

}  // namespace compass

DEFINE_JNI(ClaudeSidebarBridge)
