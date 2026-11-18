// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.base.Callback;
import org.chromium.build.annotations.NullMarked;
import org.chromium.content_public.browser.WebContents;

/**
 * Runs ExtensionUnlock's script in an extension page (extension_unlock_bridge.cc; docs/design/
 * android.md, "Extensions"): in an isolated world of the main frame, where the page's own scripts
 * can't see it.
 */
@NullMarked
@JNINamespace("compass")
final class ExtensionUnlockBridge {
    private ExtensionUnlockBridge() {}

    /** Runs |script|; the callback gets its value as JSON ("null" when there's none). */
    static void run(WebContents contents, String script, Callback<String> callback) {
        ExtensionUnlockBridgeJni.get().run(contents, script, callback);
    }

    @NativeMethods
    interface Natives {
        void run(
                @JniType("content::WebContents*") WebContents contents,
                @JniType("std::u16string") String script,
                Callback<String> callback);
    }
}
