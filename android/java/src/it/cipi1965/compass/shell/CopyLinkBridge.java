// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.build.annotations.NullMarked;
import org.chromium.chrome.browser.profiles.Profile;

/**
 * The link Copy URL puts on the clipboard (copy_link_bridge.cc; docs/design/shortcuts.md, "Clean
 * links"): without tracking parameters, unless the profile turned that off.
 */
@NullMarked
@JNINamespace("compass")
final class CopyLinkBridge {
    private CopyLinkBridge() {}

    /** |url| as Copy URL copies it for |profile|. */
    static String linkToCopy(Profile profile, String url) {
        return CopyLinkBridgeJni.get().linkToCopy(profile, url);
    }

    @NativeMethods
    interface Natives {
        @JniType("std::string")
        String linkToCopy(
                @JniType("Profile*") Profile profile, @JniType("std::string") String url);
    }
}
