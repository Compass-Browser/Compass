// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass;

import android.app.Activity;
import android.content.Intent;
import android.view.KeyEvent;

import it.cipi1965.compass.shell.CompassBar;
import it.cipi1965.compass.shell.StartPage;

import org.chromium.build.annotations.ServiceImpl;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.ui.native_page.NativePage;

/** Adds Compass's shell to each tabbed activity. */
@ServiceImpl(CompassShellHook.class)
public class CompassShellHookImpl implements CompassShellHook {
    private CompassBar mBar;

    @Override
    public void onFinishNativeInitialization(
            Activity activity, BottomControlsStacker bottomControls) {
        mBar = new CompassBar((ChromeTabbedActivity) activity, bottomControls);
        mBar.attach();
    }

    @Override
    public boolean onNewIntent(Intent intent) {
        return mBar != null && mBar.handleIntent(intent);
    }

    @Override
    public @Nullable NativePage createStartPage(Activity activity, Tab tab) {
        if (tab.isIncognito() || !(activity instanceof ChromeTabbedActivity cta)) return null;
        return new StartPage(cta, tab);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        return mBar != null && mBar.dispatchKeyEvent(event);
    }
}
