// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import static org.junit.Assert.assertEquals;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.annotation.Config;

import org.chromium.base.test.BaseRobolectricTestRunner;

/** The Downloads screen's sizes and lines (DownloadsUi). */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE)
public class DownloadsUiTest {
    private static DownloadsUi.Download download(
            DownloadsUi.Status status, long received, long total, int percent) {
        return new DownloadsUi.Download(
                "k", "file.zip", "example.com", status, received, total, percent, 0);
    }

    @Test
    public void sizes() {
        assertEquals("512 B", DownloadsUi.formatSize(512));
        assertEquals("1.5 KB", DownloadsUi.formatSize(1500));
        assertEquals("12 MB", DownloadsUi.formatSize(12_000_000));
        assertEquals("1.2 GB", DownloadsUi.formatSize(1_200_000_000L));
    }

    @Test
    public void lines() {
        assertEquals(
                "40% · 4.0 MB of 10 MB",
                DownloadsUi.subtitle(
                        download(DownloadsUi.Status.RUNNING, 4_000_000, 10_000_000, 40)));
        assertEquals(
                "Paused · 4.0 MB",
                DownloadsUi.subtitle(download(DownloadsUi.Status.PAUSED, 4_000_000, 0, -1)));
        assertEquals(
                "10 MB · example.com",
                DownloadsUi.subtitle(
                        download(DownloadsUi.Status.DONE, 10_000_000, 10_000_000, 100)));
        assertEquals("Failed", DownloadsUi.subtitle(download(DownloadsUi.Status.FAILED, 0, 0, -1)));
    }
}
