// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.profiles;

import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.base.Callback;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.profiles.Profile;

/**
 * The browser's profiles, for windows that each show one (docs/design/android.md, "Profiles"): a
 * profile by its directory's name under the user data directory, and a profile's directory name.
 */
@NullMarked
@JNINamespace("compass")
public final class ProfilesBridge {
    private ProfilesBridge() {}

    /** Loads the profile in |dir| (the directory's name), or gives null if it can't. */
    public static void loadProfile(String dir, Callback<@Nullable Profile> callback) {
        ProfilesBridgeJni.get().loadProfile(dir, callback);
    }

    /** The name of |profile|'s directory ("Default" for the first one). */
    public static String profileDir(Profile profile) {
        return ProfilesBridgeJni.get().getProfileDir(profile.getOriginalProfile());
    }

    @NativeMethods
    interface Natives {
        void loadProfile(@JniType("std::string") String dir, Callback<@Nullable Profile> callback);

        @JniType("std::string")
        String getProfileDir(@JniType("Profile*") Profile profile);
    }
}
