// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// PipBridge.java's natives (docs/design/android.md, "Auto
// picture-in-picture").

#include "base/android/jni_android.h"
#include "chrome/browser/picture_in_picture/auto_picture_in_picture_tab_helper.h"
#include "chrome/browser/picture_in_picture/auto_picture_in_picture_window_occlusion_helper_base.h"
#include "content/public/browser/web_contents.h"

// Must come after the includes it uses.
#include "compass/android/jni_headers/PipBridge_jni.h"

namespace compass {

// Android has no window occlusion helper (a stub), so the shell says when its
// window is left for another app or comes back. The tab helper treats it as
// the desktop's window being hidden or shown again, which also follows
// compass.pip.on_leaving_window (the auto_picture_in_picture_tab_helper.cc
// patch).
static void JNI_PipBridge_SetWindowLeft(JNIEnv* env,
                                        content::WebContents* contents,
                                        bool left) {
  if (!contents) {
    return;
  }
  auto* helper = AutoPictureInPictureTabHelper::FromWebContents(contents);
  if (!helper) {
    return;
  }
  using OcclusionState =
      AutoPictureInPictureWindowOcclusionHelperBase::OcclusionState;
  helper->OnOcclusionStateChanged(left ? OcclusionState::kHidden
                                       : OcclusionState::kVisible);
}

}  // namespace compass

DEFINE_JNI(PipBridge)
