// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass;

import android.app.Activity;

import it.cipi1965.compass.shell.CompassBar;

import org.chromium.build.annotations.ServiceImpl;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker;

/** Adds Compass's shell to each tabbed activity. */
@ServiceImpl(CompassShellHook.class)
public class CompassShellHookImpl implements CompassShellHook {
    @Override
    public void onFinishNativeInitialization(
            Activity activity, BottomControlsStacker bottomControls) {
        new CompassBar((ChromeTabbedActivity) activity, bottomControls).attach();
    }
}
