// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.annotation.Config;

import org.chromium.base.test.BaseRobolectricTestRunner;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** The command bar's tabs on other devices (command-bar.md, "Tabs on other devices"). */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE)
public class CommandBarStateTest {
    private static CommandBarState.DeviceTab tab(String title, String url, String device) {
        return new CommandBarState.DeviceTab(title, url, device);
    }

    private static final List<CommandBarState.Device> DEVICES =
            Arrays.asList(
                    new CommandBarState.Device(
                            "Mac",
                            100,
                            Arrays.asList(
                                    tab("Rust book", "https://doc.rust-lang.org/book/", "Mac"),
                                    tab("News", "https://news.example/", "Mac"))),
                    new CommandBarState.Device(
                            "Pixel",
                            200,
                            Arrays.asList(tab("Rust playground", "https://play.rust-lang.org/", "Pixel"))));

    private static List<String> titles(List<CommandBarState.DeviceTab> tabs) {
        List<String> titles = new ArrayList<>();
        for (CommandBarState.DeviceTab tab : tabs) titles.add(tab.title);
        return titles;
    }

    @Test
    public void everyWordMatchesTheTitleUrlOrDevice() {
        assertEquals(
                Arrays.asList("Rust playground", "Rust book"),
                titles(CommandBarState.matchDeviceTabs(DEVICES, "rust")));
        assertEquals(
                Arrays.asList("Rust book"),
                titles(CommandBarState.matchDeviceTabs(DEVICES, "Rust mac")));
        assertEquals(
                Arrays.asList("News"), titles(CommandBarState.matchDeviceTabs(DEVICES, "news.example")));
    }

    @Test
    public void nothingForAnEmptyQuery() {
        assertTrue(CommandBarState.matchDeviceTabs(DEVICES, "  ").isEmpty());
    }

    @Test
    public void atMostSix() {
        List<CommandBarState.DeviceTab> many = new ArrayList<>();
        for (int i = 0; i < 10; i++) many.add(tab("Page " + i, "https://e.example/" + i, "Mac"));
        List<CommandBarState.Device> devices =
                Arrays.asList(new CommandBarState.Device("Mac", 1, many));
        assertEquals(
                CommandBarState.MAX_DEVICE_TABS,
                CommandBarState.matchDeviceTabs(devices, "page").size());
    }
}
