// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass;

import android.app.Activity;
import android.content.Context;

import org.chromium.base.ContextUtils;
import org.chromium.base.ServiceLoaderUtil;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.ui.native_page.NativePage;

/**
 * Whether this app has Compass's shell (compass_apk does, chrome_public_apk doesn't). Chrome's
 * code asks it where the shell replaces a piece of Chrome's UI, such as the top toolbar.
 */
public final class CompassShell {
    private static Boolean sPresent;
    private static @Nullable CompassShellHook sHook;

    private CompassShell() {}

    public static boolean isPresent() {
        if (sPresent == null) {
            sHook = ServiceLoaderUtil.maybeCreate(CompassShellHook.class);
            sPresent = sHook != null;
        }
        return sPresent;
    }

    /**
     * Chrome's undo bar for closed tabs (UndoBarController patch): true if the shell shows its own
     * toast instead, which runs |undo| or, once it's gone, |commit|.
     */
    public static boolean showUndoToast(
            Context context, String text, Runnable undo, Runnable commit) {
        Activity activity = ContextUtils.activityFromContext(context);
        org.chromium.base.Log.i("CompassShell", "undo toast for %s: activity %s", text, activity);
        return isPresent()
                && sHook != null
                && activity != null
                && sHook.showUndoToast(activity, text, undo, commit);
    }

    /** The shell's start page for |tab|, or null (Chrome's new tab page). */
    public static @Nullable NativePage createStartPage(Activity activity, Tab tab) {
        return isPresent() && sHook != null ? sHook.createStartPage(activity, tab) : null;
    }
}
