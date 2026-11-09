// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass;

import android.app.Activity;
import android.content.Intent;

import org.chromium.chrome.browser.browser_controls.BottomControlsStacker;

/**
 * Lets Compass's shell into Chrome's tabbed activity. ChromeTabbedActivity looks the
 * implementation up with ServiceLoaderUtil, so builds without one (chrome_public_apk) run as
 * Chrome.
 */
public interface CompassShellHook {
    /** Called when the activity has finished its native initialization. */
    void onFinishNativeInitialization(Activity activity, BottomControlsStacker bottomControls);

    /** A new intent for the activity: true if the shell handled it, and Chrome shouldn't. */
    default boolean onNewIntent(Intent intent) {
        return false;
    }
}
