// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import static org.junit.Assert.assertEquals;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.annotation.Config;

import org.chromium.base.ContextUtils;
import org.chromium.base.test.BaseRobolectricTestRunner;

/** Settings → On Launch (P3): the stored choice. */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE)
public class OnLaunchTest {
    @Test
    public void defaultsToTheSidebar() {
        assertEquals(OnLaunch.SIDEBAR, OnLaunch.get());
    }

    @Test
    public void keepsLastTab() {
        OnLaunch.set(OnLaunch.LAST_TAB);
        assertEquals(OnLaunch.LAST_TAB, OnLaunch.get());
        OnLaunch.set(OnLaunch.SIDEBAR);
        assertEquals(OnLaunch.SIDEBAR, OnLaunch.get());
    }

    @Test
    public void readsAnythingElseAsTheSidebar() {
        ContextUtils.getAppSharedPreferences()
                .edit()
                .putString("compass_on_launch", "something")
                .apply();
        assertEquals(OnLaunch.SIDEBAR, OnLaunch.get());
        OnLaunch.set("something");
        assertEquals(OnLaunch.SIDEBAR, OnLaunch.get());
    }
}
