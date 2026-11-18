// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass;

import android.app.Activity;
import android.content.Intent;
import android.view.KeyEvent;

import it.cipi1965.compass.profiles.WindowProfiles;
import it.cipi1965.compass.shell.CompassBar;
import it.cipi1965.compass.shell.LittleWindow;
import it.cipi1965.compass.shell.StartPage;

import org.chromium.base.supplier.OneshotSupplier;
import org.chromium.build.annotations.ServiceImpl;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker;
import org.chromium.chrome.browser.profiles.ProfileProvider;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.ui.native_page.NativePage;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.mojom.WindowOpenDisposition;
import org.chromium.url.GURL;
import org.chromium.url.Origin;

/** Adds Compass's shell to each tabbed activity. */
@ServiceImpl(CompassShellHook.class)
public class CompassShellHookImpl implements CompassShellHook {
    private CompassBar mBar;
    // A link from another app that came before the shell (a cold start with it).
    private @Nullable GURL mPendingLittleWindow;

    @Override
    public void onFinishNativeInitialization(
            Activity activity, BottomControlsStacker bottomControls) {
        mBar = new CompassBar((ChromeTabbedActivity) activity, bottomControls);
        mBar.attach();
        if (mPendingLittleWindow != null) {
            mBar.openLinkFromOtherApp(mPendingLittleWindow);
            mPendingLittleWindow = null;
        }
    }

    @Override
    public boolean openLinkFromOtherApp(Intent intent) {
        String link =
                LittleWindow.linkFromOtherApp(
                        org.chromium.base.ContextUtils.getApplicationContext(), intent);
        if (link == null) return false;
        GURL url = new GURL(link);
        if (!url.isValid()) return false;
        if (mBar != null) {
            mBar.openLinkFromOtherApp(url);
        } else {
            mPendingLittleWindow = url;
        }
        return true;
    }

    // The two below run on CompassShell's own instance, for any window: the tab's window's bar.
    private static @Nullable CompassBar barOf(Tab tab) {
        if (tab.getWindowAndroid() == null) return null;
        Activity activity = tab.getWindowAndroid().getActivity().get();
        return activity != null ? CompassBar.forActivity(activity) : null;
    }

    @Override
    public boolean peekLink(Tab tab, GURL url, @Nullable Origin initiator) {
        CompassBar bar = barOf(tab);
        return bar != null && bar.peekLink(tab, url, initiator);
    }

    @Override
    public boolean peekNewContents(
            Tab tab, WebContents contents, int disposition, boolean userGesture) {
        // As the desktop's MaybePeekNewContents(): a foreground tab with a user gesture.
        if (disposition != WindowOpenDisposition.NEW_FOREGROUND_TAB || !userGesture) return false;
        CompassBar bar = barOf(tab);
        return bar != null && bar.peekNewContents(tab, contents);
    }

    @Override
    public @Nullable OneshotSupplier<ProfileProvider> createProfileProvider(
            Activity activity, int windowId, OneshotSupplier<ProfileProvider> chromes) {
        return WindowProfiles.createProvider(activity, windowId);
    }

    @Override
    public boolean onNewIntent(Intent intent) {
        return mBar != null && mBar.handleIntent(intent);
    }

    @Override
    public @Nullable NativePage createStartPage(Activity activity, Tab tab) {
        if (!(activity instanceof ChromeTabbedActivity cta)) return null;
        return new StartPage(cta, tab);
    }

    @Override
    public boolean showExtensions() {
        return mBar != null && mBar.showExtensions();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        return mBar != null && mBar.dispatchKeyEvent(event);
    }
}
