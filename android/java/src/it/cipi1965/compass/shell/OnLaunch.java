// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.content.Intent;

import org.chromium.base.ContextUtils;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tabmodel.TabModelUtils;

/**
 * Settings → On Launch, as the iOS app's (AppModel.swift, StartScreen; docs/design/android.md,
 * P3): a launch shows the sidebar (the default), or the tab last shown with the sidebar down. The
 * space a launch opens on is the one last shown (the list's active space, which the sidebar file
 * keeps), and the tab is the one Chrome restores as selected.
 */
public class OnLaunch {
    /** The sidebar over the restored tab. */
    public static final String SIDEBAR = "sidebar";

    /** The tab last shown, if it's still open; else the sidebar. */
    public static final String LAST_TAB = "last_tab";

    private static final String PREF = "compass_on_launch";

    // Whether this process has had its launch: the first window's shell runs it, once.
    private static boolean sLaunched;

    private OnLaunch() {}

    /** The setting: {@link #SIDEBAR} or {@link #LAST_TAB}. */
    public static String get() {
        String value = ContextUtils.getAppSharedPreferences().getString(PREF, SIDEBAR);
        return LAST_TAB.equals(value) ? LAST_TAB : SIDEBAR;
    }

    public static void set(String value) {
        ContextUtils.getAppSharedPreferences()
                .edit()
                .putString(PREF, LAST_TAB.equals(value) ? LAST_TAB : SIDEBAR)
                .apply();
    }

    /**
     * Runs the launch for the first window of the process: shows the sidebar unless the setting
     * is Last Tab and that tab is there. A launch for something else (a link from another app,
     * Compass's tile, widget or shortcuts) shows that instead.
     *
     * @param handled whether the shell already handled the launch intent (CompassIntents).
     */
    static void run(ChromeTabbedActivity activity, Spaces spaces, boolean handled) {
        if (sLaunched) return;
        sLaunched = true;
        if (handled || !isLauncherIntent(activity.getIntent())) return;
        if (SIDEBAR.equals(get())) {
            spaces.showSidebar();
            return;
        }
        // Last Tab: Chrome selects the tab last shown once it has restored the tabs; with none (it
        // was closed or archived), or an incognito or empty one, the sidebar.
        TabModelUtils.runOnTabStateInitialized(
                activity.getTabModelSelector(),
                selector -> {
                    if (activity.isActivityFinishingOrDestroyed()) return;
                    Tab tab = activity.getActivityTabProvider().get();
                    if (tab == null || tab.isIncognito() || isStartPage(tab)) spaces.showSidebar();
                });
    }

    private static boolean isLauncherIntent(@Nullable Intent intent) {
        if (intent == null) return true;
        String action = intent.getAction();
        return (action == null || Intent.ACTION_MAIN.equals(action)) && intent.getData() == null;
    }

    private static boolean isStartPage(Tab tab) {
        String host = tab.getUrl().getHost();
        String scheme = tab.getUrl().getScheme();
        return "newtab".equals(host) && ("chrome-native".equals(scheme) || "chrome".equals(scheme));
    }
}
