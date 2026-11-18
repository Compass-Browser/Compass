// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass;

import android.app.Activity;

import org.chromium.base.ServiceLoaderUtil;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.ui.native_page.NativePage;
import org.chromium.components.embedder_support.contextmenu.ContextMenuNativeDelegate;
import org.chromium.components.embedder_support.contextmenu.ContextMenuUi;
import org.chromium.content_public.browser.WebContents;
import org.chromium.url.GURL;
import org.chromium.url.Origin;

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

    /** The shell's start page for |tab|, or null (Chrome's new tab page). */
    public static @Nullable NativePage createStartPage(Activity activity, Tab tab) {
        return isPresent() && sHook != null ? sHook.createStartPage(activity, tab) : null;
    }

    /** "Peek" in |tab|'s link menu: true if the shell peeks |url| (CompassShellHook.peekLink). */
    public static boolean peekLink(Tab tab, GURL url, @Nullable Origin initiator) {
        return isPresent() && sHook != null && sHook.peekLink(tab, url, initiator);
    }

    /** The shell's long-press menu for |activity|, or null (Chrome's). */
    public static @Nullable ContextMenuUi createContextMenu(
            Activity activity, ContextMenuNativeDelegate nativeDelegate) {
        return isPresent() && sHook != null
                ? sHook.createContextMenu(activity, nativeDelegate)
                : null;
    }

    /** A page |tab| opens in a new tab: true if the shell peeks it (and owns |contents|). */
    public static boolean peekNewContents(
            Tab tab, WebContents contents, int disposition, boolean userGesture) {
        return isPresent()
                && sHook != null
                && sHook.peekNewContents(tab, contents, disposition, userGesture);
    }
}
