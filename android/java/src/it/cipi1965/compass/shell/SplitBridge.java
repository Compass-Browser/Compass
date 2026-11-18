// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.build.annotations.NullMarked;
import org.chromium.content_public.browser.WebContents;

/** A split's borrowed page on the native side (split_bridge.cc; android.md, "Splits"). */
@NullMarked
@JNINamespace("compass")
final class SplitBridge {
    private SplitBridge() {}

    /**
     * The page's layer goes back into its tab's content layer, which Chrome draws: the
     * ThinWebView that borrowed the page took it as its compositor's root.
     */
    static void returnLayerToTab(WebContents contents) {
        SplitBridgeJni.get().returnLayerToTab(contents);
    }

    @NativeMethods
    interface Natives {
        void returnLayerToTab(@JniType("content::WebContents*") WebContents contents);
    }
}
