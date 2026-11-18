// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ExtensionUnlockBridge.java's natives (docs/design/android.md, "Extensions").

#include <string>
#include <utility>

#include "base/android/callback_android.h"
#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

// Must come after the includes it uses.
#include "compass/android/jni_headers/ExtensionUnlockBridge_jni.h"

using base::android::JavaRef;
using base::android::ScopedJavaGlobalRef;

namespace compass {

// Only extension pages: the unlock script watches password fields, which on
// a website are the page's business (and Chrome's password manager's).
static void JNI_ExtensionUnlockBridge_Run(JNIEnv* env,
                                          content::WebContents* contents,
                                          const std::u16string& script,
                                          const JavaRef<jobject>& j_callback) {
  ScopedJavaGlobalRef<jobject> callback(j_callback);
  content::RenderFrameHost* frame =
      contents ? contents->GetPrimaryMainFrame() : nullptr;
  if (!frame || !frame->IsRenderFrameLive() ||
      !frame->GetLastCommittedURL().SchemeIs("chrome-extension")) {
    base::android::RunStringCallbackAndroid(callback, std::string("null"));
    return;
  }
  frame->ExecuteJavaScriptInIsolatedWorld(
      script,
      base::BindOnce(
          [](ScopedJavaGlobalRef<jobject> callback, base::Value result) {
            std::string json;
            base::JSONWriter::Write(result, &json);
            base::android::RunStringCallbackAndroid(callback, json);
          },
          std::move(callback)),
      ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

}  // namespace compass

DEFINE_JNI(ExtensionUnlockBridge)
