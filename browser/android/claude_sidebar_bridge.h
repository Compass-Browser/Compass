// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_CLAUDE_SIDEBAR_BRIDGE_H_
#define COMPASS_BROWSER_ANDROID_CLAUDE_SIDEBAR_BRIDGE_H_

#include <jni.h>

#include <string>

#include "base/android/scoped_java_ref.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "compass/browser/claude/claude_quick.h"
#include "compass/browser/ui/claude/link_folder_namer.h"
#include "compass/browser/ui/claude/today_tidier.h"
#include "components/prefs/pref_change_registrar.h"

class Profile;

namespace compass {

// The native half of it.cipi1965.compass.claude.ClaudeSidebarBridge, one per
// window: Claude in the sidebar on Android (docs/design/ai-sidebar.md;
// docs/design/android.md, P12), on the desktop's services. Names for link
// folders (LinkFolderNamer: Suggest a Name, and the sparkle when Claude names
// one), Tidy Today (TodayTidier), shorter tab titles (ShortTitler, which only
// needs to exist: the sidebar's snapshot carries its titles) and Ask in the
// command bar (ClaudeQuick::Stream()). Never for incognito: the window's
// regular profile only.
class ClaudeSidebarBridge : public LinkFolderNamer::Observer,
                            public TodayTidier::Observer {
 public:
  ClaudeSidebarBridge(JNIEnv* env,
                      const base::android::JavaRef<jobject>& java_object,
                      Profile* profile);
  ClaudeSidebarBridge(const ClaudeSidebarBridge&) = delete;
  ClaudeSidebarBridge& operator=(const ClaudeSidebarBridge&) = delete;
  ~ClaudeSidebarBridge() override;

  void Destroy(JNIEnv* env);
  // What's offered, as JSON: "suggest_names" (Suggest a Name: the switch on
  // and Claude set up), "tidy" (Tidy in Today's header), "busy" (the spaces
  // a tidy waits for), "ask" (Ask in the command bar).
  std::string Snapshot(JNIEnv* env);

  // Suggest a Name for a folder.
  void SuggestName(JNIEnv* env, const std::string& folder_id);
  // Tidy Today of a space; how it went comes to onTidied() or
  // onTidyFailed().
  void Tidy(JNIEnv* env, const std::string& space_id);
  // Takes a tidy back (onTidied()'s |undo_id|).
  bool UndoTidy(JNIEnv* env, int32_t undo_id);

  // Whether |text| reads as a question, in English or the browser's language.
  bool LooksLikeQuestion(JNIEnv* env, const std::u16string& text);
  // Asks |question| (with the page's |page_title| and |page_host|, when
  // "About this page" is on; empty: none); the answer streams to onAnswer(),
  // then onAnswerDone(). Returns the user message Claude gets (for Continue
  // in Chat), or empty if it couldn't ask.
  std::string Ask(JNIEnv* env,
                  const std::string& question,
                  const std::string& page_title,
                  const std::string& page_host);
  // Drops the answer on its way.
  void CancelAsk(JNIEnv* env);

  // LinkFolderNamer::Observer:
  void OnFolderNamed(const base::Uuid& folder_id) override;

  // TodayTidier::Observer:
  void OnTidyBusyChanged(const base::Uuid& space_id) override;
  void OnTidied(const base::Uuid& space_id,
                int undo_id,
                size_t folders) override;
  void OnTidyFailed(const base::Uuid& space_id,
                    const std::string& message) override;

 private:
  void Changed();
  void OnAnswerText(int id, const std::string& text);
  void OnAnswerDone(int id, claude::ClaudeQuick::Result result);

  base::android::ScopedJavaGlobalRef<jobject> java_object_;
  raw_ptr<Profile> profile_;
  raw_ptr<claude::ClaudeQuick> quick_;
  // The answer on its way (ClaudeQuick::Stream()'s id), or 0.
  int ask_id_ = 0;
  PrefChangeRegistrar prefs_;
  base::ScopedObservation<LinkFolderNamer, LinkFolderNamer::Observer>
      namer_observation_{this};
  base::ScopedObservation<TodayTidier, TodayTidier::Observer>
      tidier_observation_{this};
  base::WeakPtrFactory<ClaudeSidebarBridge> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_CLAUDE_SIDEBAR_BRIDGE_H_
