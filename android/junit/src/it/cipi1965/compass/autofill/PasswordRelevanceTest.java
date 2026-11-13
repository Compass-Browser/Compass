// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.autofill;

import static org.junit.Assert.assertEquals;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.annotation.Config;

import org.chromium.base.test.BaseRobolectricTestRunner;

/** Which saved passwords the autofill picker lists first for a form (PasswordPickerActivity). */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE)
public class PasswordRelevanceTest {
    private static int relevance(String site, String webDomain, String packageName) {
        return PasswordPickerActivity.Companion.relevance(site, webDomain, packageName);
    }

    @Test
    public void webDomains() {
        assertEquals(3, relevance("https://www.example.com/", "example.com", "com.browser"));
        assertEquals(2, relevance("https://login.example.com/", "example.com", "com.browser"));
        assertEquals(0, relevance("https://other.org/", "example.com", "com.browser"));
    }

    @Test
    public void appsByTheirPackagesWords() {
        assertEquals(1, relevance("https://example.com/", null, "com.example.app"));
        assertEquals(0, relevance("https://other.org/", null, "com.example.app"));
    }

    @Test
    public void loginsSavedFromAnApp() {
        String saved = "android://aGFzaA==@com.example.app/";
        assertEquals(3, relevance(saved, null, "com.example.app"));
        assertEquals(0, relevance(saved, null, "com.other.app"));
    }
}
