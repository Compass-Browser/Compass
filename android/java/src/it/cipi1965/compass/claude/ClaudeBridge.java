// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.claude;

import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.content_public.browser.WebContents;

/**
 * Java side of the Claude chat (//compass/browser/android: ClaudeBridge, on the desktop's Claude
 * client): the conversation as JSON snapshots, questions and the key as calls.
 */
@JNINamespace("compass")
public class ClaudeBridge {
    private long mNativeClaudeBridge;
    private final Runnable mOnChanged;

    public ClaudeBridge(Profile profile, Runnable onChanged) {
        mOnChanged = onChanged;
        mNativeClaudeBridge = ClaudeBridgeJni.get().init(this, profile);
    }

    public void destroy() {
        if (mNativeClaudeBridge == 0) return;
        ClaudeBridgeJni.get().destroy(mNativeClaudeBridge);
        mNativeClaudeBridge = 0;
    }

    @CalledByNative
    private void onChanged() {
        mOnChanged.run();
    }

    public String snapshot() {
        return ClaudeBridgeJni.get().snapshot(mNativeClaudeBridge);
    }

    public void setApiKey(String key) {
        ClaudeBridgeJni.get().setApiKey(mNativeClaudeBridge, key);
    }

    /** Asks |text| about the page |contents| shows. */
    public void send(String text, @Nullable WebContents contents) {
        ClaudeBridgeJni.get().send(mNativeClaudeBridge, text, contents);
    }

    /** "Let Claude use the whole browser": the tabs, not only the page. */
    public void setBrowserScope(boolean allowed) {
        ClaudeBridgeJni.get().setBrowserScope(mNativeClaudeBridge, allowed);
    }

    public void stop() {
        ClaudeBridgeJni.get().stop(mNativeClaudeBridge);
    }

    public void clear() {
        ClaudeBridgeJni.get().clear(mNativeClaudeBridge);
    }

    @NativeMethods
    interface Natives {
        long init(ClaudeBridge caller, @JniType("Profile*") Profile profile);

        void destroy(long nativeClaudeBridge);

        @JniType("std::string")
        String snapshot(long nativeClaudeBridge);

        void setApiKey(long nativeClaudeBridge, @JniType("std::string") String key);

        void send(
                long nativeClaudeBridge,
                @JniType("std::string") String text,
                @JniType("content::WebContents*") @Nullable WebContents contents);

        void setBrowserScope(long nativeClaudeBridge, boolean allowed);

        void stop(long nativeClaudeBridge);

        void clear(long nativeClaudeBridge);
    }
}
