// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.profiles;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityOptions;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.provider.Browser;

import it.cipi1965.compass.extras.CompassIntents;

import org.chromium.base.ApplicationStatus;
import org.chromium.base.IntentUtils;
import org.chromium.base.supplier.OneshotSupplier;
import org.chromium.base.supplier.OneshotSupplierImpl;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.IntentHandler;
import org.chromium.chrome.browser.multiwindow.MultiWindowUtils;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.profiles.ProfileProvider;
import org.chromium.chrome.browser.tabwindow.TabWindowManager;

import java.util.Map;

/**
 * Windows and profiles (docs/design/android.md, "Profiles"): Chrome on Android gives a window one
 * regular profile, so each window shows one profile's spaces. Showing a space of another profile
 * brings that profile's window forward in its place, as the desktop's window swap
 * (profiles-in-place.md), and the window left behind leaves Recents until it's shown again.
 *
 * <p>A window's profile is its directory's name, kept by window ID; a window without one is the
 * last used regular profile's (Chrome's).
 */
@NullMarked
public final class WindowProfiles {
    /** The profile (its directory's name) a new or relaunched window is for. */
    public static final String EXTRA_PROFILE_DIR = "it.cipi1965.compass.extra.PROFILE_DIR";

    private static final String PREFS = "compass_window_profiles";

    private static boolean sObserving;

    private WindowProfiles() {}

    /** The profile window |windowId| shows, or null for Chrome's own. */
    public static @Nullable OneshotSupplier<ProfileProvider> createProvider(
            Activity activity, int windowId, OneshotSupplier<ProfileProvider> chromes) {
        observeWindows();
        SharedPreferences prefs = prefs(activity);
        Intent intent = activity.getIntent();
        String dir = intent != null ? IntentUtils.safeGetStringExtra(intent, EXTRA_PROFILE_DIR) : null;
        if (dir != null) {
            prefs.edit().putString(key(windowId), dir).apply();
        } else {
            dir = prefs.getString(key(windowId), null);
        }
        if (dir == null || dir.isEmpty()) return null;
        final String wanted = dir;
        OneshotSupplierImpl<ProfileProvider> provider = new OneshotSupplierImpl<>();
        chromes.onAvailable(
                chrome -> {
                    if (wanted.equals(ProfilesBridge.profileDir(chrome.getOriginalProfile()))) {
                        provider.set(chrome);
                        return;
                    }
                    ProfilesBridge.loadProfile(
                            wanted,
                            profile -> {
                                if (profile == null) {
                                    // Gone (deleted on this device): the window is Chrome's again.
                                    prefs.edit().remove(key(windowId)).apply();
                                    provider.set(chrome);
                                } else {
                                    provider.set(new BoundProvider(profile));
                                }
                            });
                });
        return provider;
    }

    /**
     * Shows |spaceId| of the profile in |dir| in place of |from|'s window: in a window of that
     * profile, brought forward, or a new one.
     */
    public static void show(ChromeTabbedActivity from, String dir, String spaceId) {
        Intent intent = new Intent(CompassIntents.ACTION_OPEN_SPACE);
        intent.putExtra(CompassIntents.EXTRA_SPACE_ID, spaceId);
        intent.putExtra(EXTRA_PROFILE_DIR, dir);
        int windowId = findWindow(from, dir);
        Context context = from.getApplicationContext();
        if (windowId != TabWindowManager.INVALID_WINDOW_ID) {
            intent.setClass(context, ChromeTabbedActivity.class);
            MultiWindowUtils.launchIntentInMaybeClosedWindow(context, intent, windowId);
        } else {
            // As Chrome's own new windows (MultiWindowUtils.createNewWindowIntent()).
            intent.setClass(context, ChromeTabbedActivity.class);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK);
            intent.putExtra(IntentHandler.EXTRA_PREFER_NEW, true);
            intent.putExtra(Browser.EXTRA_APPLICATION_ID, context.getPackageName());
            IntentUtils.addTrustedIntentExtras(intent);
            from.startActivity(
                    intent, ActivityOptions.makeCustomAnimation(from, 0, 0).toBundle());
        }
        // The window left behind isn't one more window in Recents.
        setInRecents(from, false);
    }

    /** A window of the profile in |dir|: a shown one first, then one closed with its tabs. */
    private static int findWindow(ChromeTabbedActivity from, String dir) {
        int fallback = TabWindowManager.INVALID_WINDOW_ID;
        for (Activity activity : ApplicationStatus.getRunningActivities()) {
            if (!(activity instanceof ChromeTabbedActivity window) || window == from) continue;
            String shown = windowDir(window);
            if (dir.equals(shown)) return window.getWindowId();
        }
        for (Map.Entry<String, ?> entry : prefs(from).getAll().entrySet()) {
            if (dir.equals(entry.getValue()) && entry.getKey().startsWith("w")) {
                try {
                    int id = Integer.parseInt(entry.getKey().substring(1));
                    if (id != from.getWindowId()) fallback = id;
                } catch (NumberFormatException e) {
                    // Not one of ours.
                }
            }
        }
        return fallback;
    }

    /** The directory of |window|'s profile, or null before it has one. */
    private static @Nullable String windowDir(ChromeTabbedActivity window) {
        ProfileProvider provider = window.getProfileProviderSupplier().get();
        return provider != null ? ProfilesBridge.profileDir(provider.getOriginalProfile()) : null;
    }

    // A shown window is in Recents again.
    private static void observeWindows() {
        if (sObserving) return;
        sObserving = true;
        ApplicationStatus.registerStateListenerForAllActivities(
                (activity, state) -> {
                    if (state == org.chromium.base.ActivityState.RESUMED
                            && activity instanceof ChromeTabbedActivity) {
                        setInRecents(activity, true);
                    }
                });
    }

    private static void setInRecents(Activity activity, boolean shown) {
        ActivityManager manager =
                (ActivityManager) activity.getSystemService(Context.ACTIVITY_SERVICE);
        if (manager == null) return;
        int taskId = activity.getTaskId();
        for (ActivityManager.AppTask task : manager.getAppTasks()) {
            try {
                if (task.getTaskInfo().taskId == taskId) {
                    task.setExcludeFromRecents(!shown);
                    return;
                }
            } catch (IllegalArgumentException e) {
                // The task went meanwhile.
            }
        }
    }

    private static SharedPreferences prefs(Context context) {
        return context.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private static String key(int windowId) {
        return "w" + windowId;
    }

    /** A window's profile other than Chrome's: its incognito is that profile's. */
    private static final class BoundProvider implements ProfileProvider {
        private final Profile mProfile;

        BoundProvider(Profile profile) {
            mProfile = profile;
        }

        @Override
        public Profile getOriginalProfile() {
            return mProfile;
        }

        @Override
        public @Nullable Profile getOffTheRecordProfile(boolean createIfNeeded) {
            return mProfile.getPrimaryOtrProfile(createIfNeeded);
        }
    }
}
