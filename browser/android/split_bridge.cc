// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// SplitBridge.java's natives (docs/design/android.md, "Splits").

#include "base/android/jni_android.h"
#include "cc/slim/layer.h"
#include "chrome/browser/android/tab_android.h"
#include "content/public/browser/web_contents.h"
#include "ui/android/view_android.h"

// Must come after the includes it uses.
#include "compass/android/jni_headers/SplitBridge_jni.h"

namespace compass {

// A split's other pane borrows its page into a ThinWebView, whose compositor
// takes the page's layer as its root; the page's tab put that layer in its
// content layer once, when it made it, so a page Chrome had drawn would come
// back empty. It goes back there.
static void JNI_SplitBridge_ReturnLayerToTab(JNIEnv* env,
                                             content::WebContents* contents) {
  if (!contents) {
    return;
  }
  TabAndroid* tab = TabAndroid::FromWebContents(contents);
  if (tab) {
    contents->GetNativeView()->GetLayer()->RemoveFromParent();
    tab->AttachWebContentsToContentLayer(contents);
  }
}

}  // namespace compass

DEFINE_JNI(SplitBridge)
