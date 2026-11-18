// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.provider.Browser;

import it.cipi1965.compass.extras.CompassIntents;

import org.junit.After;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.annotation.Config;

import org.chromium.base.ContextUtils;
import org.chromium.base.test.BaseRobolectricTestRunner;
import org.chromium.base.test.util.Features.DisableFeatures;
import org.chromium.chrome.browser.IntentHandler;
import org.chromium.components.external_intents.ExternalIntentsFeatures;

/** Little windows (P15): which links from other apps open in one, and the setting. */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE)
// Chrome's getTabOpenType() reads it for links that name an app.
@DisableFeatures(ExternalIntentsFeatures.DONT_CLOBBER_TABS_WITH_CHROME_APP_ID_NAME)
public class LittleWindowTest {
    private final Context mContext = ContextUtils.getApplicationContext();

    @After
    public void tearDown() {
        LittleWindow.setOn(true);
    }

    private static Intent view(String url) {
        return new Intent(Intent.ACTION_VIEW, Uri.parse(url));
    }

    @Test
    public void onByDefault() {
        assertTrue(LittleWindow.isOn());
        LittleWindow.setOn(false);
        assertFalse(LittleWindow.isOn());
    }

    @Test
    public void takesWebLinksFromOtherApps() {
        assertEquals(
                "https://example.com/a",
                LittleWindow.linkFromOtherApp(mContext, view("https://example.com/a")));
        assertEquals(
                "http://example.com/",
                LittleWindow.linkFromOtherApp(mContext, view("http://example.com/")));
        // An app that names itself: Chrome would reuse its tab; here a second link replaces the
        // first in the card.
        Intent fromApp = view("https://example.com/b");
        fromApp.putExtra(Browser.EXTRA_APPLICATION_ID, "com.example.mail");
        assertEquals(
                "https://example.com/b", LittleWindow.linkFromOtherApp(mContext, fromApp));
    }

    @Test
    public void leavesTheRestToChrome() {
        assertNull(LittleWindow.linkFromOtherApp(mContext, null));
        assertNull(LittleWindow.linkFromOtherApp(mContext, view("file:///sdcard/a.html")));
        assertNull(LittleWindow.linkFromOtherApp(mContext, view("mailto:a@example.com")));
        assertNull(
                LittleWindow.linkFromOtherApp(
                        mContext, new Intent(Intent.ACTION_MAIN, Uri.parse("https://a.com"))));
        // Compass's own: its package as the app, its widgets and shortcuts, incognito.
        Intent own = view("https://example.com/");
        own.putExtra(Browser.EXTRA_APPLICATION_ID, mContext.getPackageName());
        assertNull(LittleWindow.linkFromOtherApp(mContext, own));
        Intent shortcut = view("https://example.com/");
        shortcut.putExtra(IntentHandler.EXTRA_INVOKED_FROM_SHORTCUT, true);
        assertNull(LittleWindow.linkFromOtherApp(mContext, shortcut));
        Intent incognito = view("https://example.com/");
        incognito.putExtra(IntentHandler.EXTRA_OPEN_NEW_INCOGNITO_TAB, true);
        assertNull(LittleWindow.linkFromOtherApp(mContext, incognito));
        Intent compass = view("https://example.com/");
        compass.setAction(CompassIntents.ACTION_OPEN_ENTRY);
        assertNull(LittleWindow.linkFromOtherApp(mContext, compass));
    }

    @Test
    public void offOpensTabs() {
        LittleWindow.setOn(false);
        assertNull(LittleWindow.linkFromOtherApp(mContext, view("https://example.com/")));
    }
}
