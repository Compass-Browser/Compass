// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.provider.Browser;
import android.text.TextUtils;

import it.cipi1965.compass.extras.CompassIntents;

import org.chromium.base.ContextUtils;
import org.chromium.base.IntentUtils;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.IntentHandler;
import org.chromium.chrome.browser.IntentHandler.TabOpenType;
import org.chromium.chrome.browser.tab.Tab;

/**
 * Little windows (docs/design/android.md, "Little windows"; ios.md, "Little windows"): with
 * Compass as the default browser, a web link from another app opens in Peek's card over what the
 * window shows, not among the tabs. Settings → Open in a Little Window turns it off.
 */
public final class LittleWindow {
    private static final String PREF = "compass_little_window";

    private LittleWindow() {}

    /**
     * Settings → Open in a Little Window (on by default, as on iOS). Off, a link from another app
     * without a rule opens as a new Today tab of the shown space (CompassBar).
     */
    public static boolean isOn() {
        return ContextUtils.getAppSharedPreferences().getBoolean(PREF, true);
    }

    public static void setOn(boolean on) {
        ContextUtils.getAppSharedPreferences().edit().putBoolean(PREF, on).apply();
    }

    /**
     * |intent|'s web link, if it's a link from another app, which Compass routes itself (a rule's
     * space, else the little window or a Today tab): a VIEW intent for an http(s) page that Chrome
     * would open in a new tab, not from Compass itself (its own intents, Chrome's trusted ones,
     * its widgets and shortcuts) and not for incognito.
     */
    public static @Nullable String linkFromOtherApp(Context context, @Nullable Intent intent) {
        if (intent == null) return null;
        if (!Intent.ACTION_VIEW.equals(intent.getAction())) return null;
        if (CompassIntents.isCompassIntent(intent)) return null;
        if (IntentHandler.wasIntentSenderChrome(intent)) return null;
        String appId = IntentUtils.safeGetStringExtra(intent, Browser.EXTRA_APPLICATION_ID);
        if (TextUtils.equals(appId, context.getPackageName())) return null;
        if (IntentHandler.hasAnyIncognitoExtra(intent.getExtras())) return null;
        if (IntentHandler.getTabId(intent) != Tab.INVALID_TAB_ID) return null;
        if (IntentUtils.safeGetBooleanExtra(intent, IntentHandler.EXTRA_INVOKED_FROM_SHORTCUT, false)
                || IntentUtils.safeGetBooleanExtra(
                        intent, IntentHandler.EXTRA_INVOKED_FROM_APP_WIDGET, false)) {
            return null;
        }
        int openType = IntentHandler.getTabOpenType(intent);
        if (openType != TabOpenType.OPEN_NEW_TAB
                && openType != TabOpenType.REUSE_APP_ID_MATCHING_TAB_ELSE_NEW_TAB) {
            return null;
        }
        String url = IntentHandler.getUrlFromIntent(intent);
        if (url == null) return null;
        String scheme = Uri.parse(url).getScheme();
        return "http".equalsIgnoreCase(scheme) || "https".equalsIgnoreCase(scheme) ? url : null;
    }
}
