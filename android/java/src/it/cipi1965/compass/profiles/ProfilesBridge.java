// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.profiles;

import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

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

    /**
     * The profile in |dir| (its directory's name), loading it now if it isn't loaded (it blocks):
     * for a window of it, which Chrome expects its profile for at once. Null if there's no such
     * profile.
     */
    public static @Nullable Profile loadProfileSync(String dir) {
        return ProfilesBridgeJni.get().loadProfileSync(dir);
    }

    /** The name of |profile|'s directory ("Default" for the first one). */
    public static String profileDir(Profile profile) {
        return ProfilesBridgeJni.get().getProfileDir(profile.getOriginalProfile());
    }

    @NativeMethods
    interface Natives {
        @JniType("std::string")
        String getProfileDir(@JniType("Profile*") Profile profile);

        @Nullable Profile loadProfileSync(@JniType("std::string") String dir);
    }
}
