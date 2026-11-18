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

/** Reader mode's native side (reader_bridge.cc; docs/design/android.md, "Reader mode"). */
@NullMarked
@JNINamespace("compass")
final class ReaderBridge {
    private ReaderBridge() {}

    /** Whether |contents|' page reads as an article (reader.js check()). */
    static void check(WebContents contents, Callback<Boolean> callback) {
        ReaderBridgeJni.get().check(contents, callback);
    }

    /** Parses the page; the callback gets the article's token for {@link #pageUrl}. */
    static void parse(WebContents contents, Callback<String> callback) {
        ReaderBridgeJni.get().parse(contents, callback);
    }

    /** The reader page for the article |token| names. */
    static String pageUrl(String token) {
        return ReaderBridgeJni.get().pageUrl(token);
    }

    /** Drops the article |token| names. */
    static void release(String token) {
        ReaderBridgeJni.get().release(token);
    }

    @NativeMethods
    interface Natives {
        void check(
                @JniType("content::WebContents*") WebContents contents,
                Callback<Boolean> callback);

        void parse(
                @JniType("content::WebContents*") WebContents contents,
                Callback<String> callback);

        @JniType("std::string")
        String pageUrl(@JniType("std::string") String token);

        void release(@JniType("std::string") String token);
    }
}
