// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass;

import org.chromium.base.ServiceLoaderUtil;

/**
 * Whether this app has Compass's shell (compass_apk does, chrome_public_apk doesn't). Chrome's
 * code asks it where the shell replaces a piece of Chrome's UI, such as the top toolbar.
 */
public final class CompassShell {
    private static Boolean sPresent;

    private CompassShell() {}

    public static boolean isPresent() {
        if (sPresent == null) {
            sPresent = ServiceLoaderUtil.maybeCreate(CompassShellHook.class) != null;
        }
        return sPresent;
    }
}
