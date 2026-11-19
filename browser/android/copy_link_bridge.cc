// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CopyLinkBridge.java's natives (docs/design/shortcuts.md, "Clean links").

#include <string>

#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/common/clean_url.h"
#include "components/prefs/pref_service.h"
#include "url/gurl.h"

// Must come after the includes it uses.
#include "compass/android/jni_headers/CopyLinkBridge_jni.h"

namespace compass {

// Android copies the page's URL as it is (no compass:// there), so only the
// tracking parameters go, when the profile's pref says so.
static std::string JNI_CopyLinkBridge_LinkToCopy(JNIEnv* env,
                                                 Profile* profile,
                                                 const std::string& url) {
  if (!profile || !profile->GetPrefs()->GetBoolean(
                      prefs::kRemoveTrackingFromCopiedLinks)) {
    return url;
  }
  const GURL gurl(url);
  if (!gurl.is_valid()) {
    return url;
  }
  return RemoveTrackingParams(gurl).spec();
}

}  // namespace compass

DEFINE_JNI(CopyLinkBridge)
