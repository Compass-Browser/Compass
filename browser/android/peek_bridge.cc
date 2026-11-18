// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// PeekBridge.java's natives (docs/design/android.md, "Peek").

#include "base/android/jni_android.h"
#include "components/blocked_content/popup_blocker_tab_helper.h"
#include "content/public/browser/web_contents.h"

// Must come after the includes it uses.
#include "compass/android/jni_headers/PeekBridge_jni.h"

namespace compass {

// The card's page is a tab for the performance manager already
// (ChromeThinWebViewInitializer::AttachTabHelpers()); TabFeatures, when a tab
// adopts it on Open, marks it again unless it finds the popup blocker it
// attaches itself, which it takes as a page it has seen
// (chrome/browser/android/tab_features.cc). The page gets that now, which
// also blocks its popups in the card as in a tab.
static void JNI_PeekBridge_PrepareForTab(JNIEnv* env,
                                         content::WebContents* contents) {
  if (contents) {
    blocked_content::PopupBlockerTabHelper::CreateForWebContents(contents);
  }
}

}  // namespace compass

DEFINE_JNI(PeekBridge)
