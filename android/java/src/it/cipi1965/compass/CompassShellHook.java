// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass;

import android.app.Activity;
import android.content.Intent;
import android.view.KeyEvent;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.ui.native_page.NativePage;

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

    /**
     * The shell's start page for |tab| in |activity|, in place of Chrome's new tab page; null for
     * Chrome's. Called on an instance of its own (CompassShell), maybe before the activity's.
     */
    default @Nullable NativePage createStartPage(Activity activity, Tab tab) {
        return null;
    }

    /** Chrome's menu asked for the extensions menu: true if the shell showed its own. */
    default boolean showExtensions() {
        return false;
    }

    /** A key event for the activity: true if it was one of the shell's shortcuts. */
    default boolean dispatchKeyEvent(KeyEvent event) {
        return false;
    }
}
