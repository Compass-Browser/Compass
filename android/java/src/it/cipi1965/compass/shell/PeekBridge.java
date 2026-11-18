// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.build.annotations.NullMarked;
import org.chromium.content_public.browser.WebContents;

/** Peek's page on the native side (peek_bridge.cc; docs/design/android.md, "Peek"). */
@NullMarked
@JNINamespace("compass")
final class PeekBridge {
    private PeekBridge() {}

    /**
     * Readies the card's page for a tab to adopt it later: ThinWebView already made it a tab for
     * the performance manager, which a tab's TabFeatures would do again (a CHECK) unless the page
     * has the popup blocker TabFeatures attaches with it. The card's page gets it now.
     */
    static void prepareForTab(WebContents contents) {
        PeekBridgeJni.get().prepareForTab(contents);
    }

    @NativeMethods
    interface Natives {
        void prepareForTab(@JniType("content::WebContents*") WebContents contents);
    }
}
