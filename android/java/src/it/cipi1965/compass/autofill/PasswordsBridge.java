// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.autofill;

import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.base.Callback;
import org.chromium.chrome.browser.profiles.Profile;

/**
 * Java side of the saved passwords for the autofill picker (//compass/browser/android:
 * PasswordsBridge): each one's site and username as JSON; a password only for the one picked.
 */
@JNINamespace("compass")
public class PasswordsBridge {
    private long mNativePasswordsBridge;
    private final Callback<String> mOnLoaded;

    /** |onLoaded| gets a JSON list of {"site", "url", "username"}, in passwordAt()'s order. */
    public PasswordsBridge(Profile profile, Callback<String> onLoaded) {
        mOnLoaded = onLoaded;
        mNativePasswordsBridge = PasswordsBridgeJni.get().init(this, profile);
    }

    public void destroy() {
        if (mNativePasswordsBridge == 0) return;
        PasswordsBridgeJni.get().destroy(mNativePasswordsBridge);
        mNativePasswordsBridge = 0;
    }

    public void load() {
        PasswordsBridgeJni.get().load(mNativePasswordsBridge);
    }

    public String passwordAt(int index) {
        return PasswordsBridgeJni.get().passwordAt(mNativePasswordsBridge, index);
    }

    @CalledByNative
    private void onLoaded(@JniType("std::string") String json) {
        mOnLoaded.onResult(json);
    }

    @NativeMethods
    interface Natives {
        long init(PasswordsBridge caller, @JniType("Profile*") Profile profile);

        void destroy(long nativePasswordsBridge);

        void load(long nativePasswordsBridge);

        @JniType("std::u16string")
        String passwordAt(long nativePasswordsBridge, int index);
    }
}
