// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.build.annotations.NullMarked;
import org.chromium.content_public.browser.WebContents;

/**
 * Auto picture-in-picture when Compass is left for another app (pip_bridge.cc;
 * docs/design/android.md, "Auto picture-in-picture"): the desktop's "leaving the window".
 */
@NullMarked
@JNINamespace("compass")
final class PipBridge {
    private PipBridge() {}

    /**
     * The window showing |contents| (its active tab) was left for another app, or is back: its
     * playing video goes into picture-in-picture, or comes back to the page.
     */
    static void setWindowLeft(WebContents contents, boolean left) {
        PipBridgeJni.get().setWindowLeft(contents, left);
    }

    @NativeMethods
    interface Natives {
        void setWindowLeft(@JniType("content::WebContents*") WebContents contents, boolean left);
    }
}
