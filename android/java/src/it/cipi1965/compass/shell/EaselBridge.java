// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.base.Callback;
import org.chromium.build.annotations.NullMarked;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.content_public.browser.WebContents;

/** Easels' native side (easel_bridge.cc; docs/design/android.md, "Easels"). */
@NullMarked
@JNINamespace("compass")
final class EaselBridge {
    private EaselBridge() {}

    /** The boards as JSON, [{id, title}], the last used first. */
    static String easels(Profile profile) {
        return EaselBridgeJni.get().easels(profile);
    }

    /** A new board pinned in |spaceId|: {nodeId, url} of its entry, or {error}. */
    static String newEasel(Profile profile, String spaceId) {
        return EaselBridgeJni.get().newEasel(profile, spaceId);
    }

    /**
     * Adds the visible page to |easelId| (empty: a new board, pinned in |spaceId|); the callback
     * gets {title} of the board, or {error}.
     */
    static void capture(
            WebContents contents, String easelId, String spaceId, Callback<String> callback) {
        EaselBridgeJni.get().capture(contents, easelId, spaceId, callback);
    }

    @NativeMethods
    interface Natives {
        @JniType("std::string")
        String easels(@JniType("Profile*") Profile profile);

        @JniType("std::string")
        String newEasel(
                @JniType("Profile*") Profile profile, @JniType("std::string") String spaceId);

        void capture(
                @JniType("content::WebContents*") WebContents contents,
                @JniType("std::string") String easelId,
                @JniType("std::string") String spaceId,
                Callback<String> callback);
    }
}
