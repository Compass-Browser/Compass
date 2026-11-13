// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.annotation.Config;

import org.chromium.base.test.BaseRobolectricTestRunner;

import java.util.List;

/**
 * The sidebar's state from SpacesAndroid's snapshot (spaces_android.h): the archive (P1), pinned
 * URLs (P2) and the roots drops go to (P5); and where a dragged entry drops.
 */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE)
public class SidebarStateTest {
    // Pinned: a folder (open, one tab) and a tab away from its pinned URL; Today: two tabs.
    private static final String SNAPSHOT =
            "{\"loaded\": true, \"active\": \"s1\","
                    + " \"spaces\": [{\"id\": \"s1\", \"name\": \"Home\", \"tint\": \"\","
                    + " \"emoji\": \"\"}],"
                    + " \"pinned_id\": \"P\", \"today_id\": \"T\", \"favorites_id\": \"F\","
                    + " \"favorites\": [],"
                    + " \"pinned\": ["
                    + "  {\"id\": \"f1\", \"type\": \"folder\", \"title\": \"Work\", \"url\": \"\","
                    + "   \"collapsed\": false, \"tab\": -1, \"children\": ["
                    + "    {\"id\": \"a\", \"type\": \"tab\", \"title\": \"A\","
                    + "     \"url\": \"https://a.example/\", \"tab\": -1,"
                    + "     \"saved_url\": \"https://a.example/\"}]},"
                    + "  {\"id\": \"b\", \"type\": \"tab\", \"title\": \"B\","
                    + "   \"url\": \"https://b.example/page\", \"tab\": 7,"
                    + "   \"saved_url\": \"https://b.example/\"}],"
                    + " \"today\": ["
                    + "  {\"id\": \"c\", \"type\": \"tab\", \"title\": \"C\","
                    + "   \"url\": \"https://c.example/\", \"tab\": 8},"
                    + "  {\"id\": \"d\", \"type\": \"tab\", \"title\": \"D\","
                    + "   \"url\": \"https://d.example/\", \"tab\": -1}],"
                    + " \"archive\": ["
                    + "  {\"id\": \"x\", \"title\": \"X\", \"url\": \"https://x.example/\","
                    + "   \"archived_at\": 1700000000000.0},"
                    + "  {\"id\": \"y\", \"title\": \"Y \\u00b7 Z\", \"url\": \"https://y.example/\","
                    + "   \"archived_at\": 1600000000000.0}],"
                    + " \"archive_after_hours\": 168}";

    private static SidebarState state() {
        SidebarState state = new SidebarState();
        state.update(SNAPSHOT, 8);
        return state;
    }

    @Test
    public void readsTheArchive() {
        SidebarState state = state();
        List<SidebarState.ArchivedEntry> archive = state.getArchive();
        assertEquals(2, archive.size());
        assertEquals("x", archive.get(0).id);
        assertEquals("https://x.example/", archive.get(0).url);
        assertEquals(1700000000000L, archive.get(0).archivedAt);
        assertEquals("Y · Z", archive.get(1).title);
        assertEquals(168, state.getArchiveAfterHours());
    }

    @Test
    public void archiveAfterDefaultsToTwelveHours() {
        SidebarState state = new SidebarState();
        state.update("{\"loaded\": true}", -1);
        assertEquals(12, state.getArchiveAfterHours());
        assertTrue(state.getArchive().isEmpty());
    }

    @Test
    public void knowsWhenAnEntryIsAwayFromItsPinnedUrl() {
        SidebarState state = state();
        SidebarState.Node folder = state.getPinned().get(0);
        SidebarState.Node b = state.getPinned().get(1);
        assertFalse(folder.children.get(0).isAwayFromSavedUrl());
        assertTrue(b.isAwayFromSavedUrl());
        assertEquals("https://b.example/", b.savedUrl);
        // Today entries have no pinned URL.
        assertFalse(state.getToday().get(0).isAwayFromSavedUrl());
    }

    @Test
    public void readsTheRoots() {
        SidebarState state = state();
        assertEquals("P", state.getPinnedId());
        assertEquals("T", state.getTodayId());
        assertEquals("F", state.getFavoritesId());
    }

    private static List<SidebarUi.ListRow> rows() {
        // As SpacePage lays them out: 40 px headers, 44 px rows.
        return List.of(
                new SidebarUi.ListRow("header", 0, 40),
                new SidebarUi.ListRow("pinned-header", 40, 40),
                new SidebarUi.ListRow("f1", 80, 44),
                new SidebarUi.ListRow("a", 124, 44),
                new SidebarUi.ListRow("b", 168, 44),
                new SidebarUi.ListRow("today-header", 212, 40),
                new SidebarUi.ListRow("new-tab", 252, 44),
                new SidebarUi.ListRow("c", 296, 44),
                new SidebarUi.ListRow("d", 340, 44));
    }

    @Test
    public void dropsAboveAndBelowEntries() {
        SidebarState state = state();
        // The upper half of B: before it, in Pinned.
        SidebarUi.DropTarget above = SidebarUi.dropTarget(state, rows(), "d", 170f);
        assertNotNull(above);
        assertEquals("P", above.parentId);
        assertEquals("b", above.beforeId);
        assertEquals(SidebarUi.Edge.TOP, above.edge);
        // The lower half of C: before D, in Today.
        SidebarUi.DropTarget below = SidebarUi.dropTarget(state, rows(), "b", 330f);
        assertNotNull(below);
        assertEquals("T", below.parentId);
        assertEquals("d", below.beforeId);
        assertEquals(SidebarUi.Edge.BOTTOM, below.edge);
        // The lower half of the last entry: the end of its parent.
        SidebarUi.DropTarget end = SidebarUi.dropTarget(state, rows(), "b", 380f);
        assertNotNull(end);
        assertEquals("T", end.parentId);
        assertEquals("", end.beforeId);
    }

    @Test
    public void dropsIntoFoldersAndSections() {
        SidebarState state = state();
        // The middle of the folder: into it, at its end.
        SidebarUi.DropTarget into = SidebarUi.dropTarget(state, rows(), "c", 102f);
        assertNotNull(into);
        assertEquals("f1", into.parentId);
        assertEquals("", into.beforeId);
        assertEquals(SidebarUi.Edge.INSIDE, into.edge);
        // Below the open folder: its first entry.
        SidebarUi.DropTarget top = SidebarUi.dropTarget(state, rows(), "c", 120f);
        assertNotNull(top);
        assertEquals("f1", top.parentId);
        assertEquals("a", top.beforeId);
        // In a folder: before its entry, in the folder.
        SidebarUi.DropTarget inFolder = SidebarUi.dropTarget(state, rows(), "c", 126f);
        assertNotNull(inFolder);
        assertEquals("f1", inFolder.parentId);
        assertEquals("a", inFolder.beforeId);
        // Today's header and New Tab: the top of Today.
        SidebarUi.DropTarget today = SidebarUi.dropTarget(state, rows(), "b", 260f);
        assertNotNull(today);
        assertEquals("T", today.parentId);
        assertEquals("c", today.beforeId);
        // Pinned's header: the top of Pinned.
        SidebarUi.DropTarget pinned = SidebarUi.dropTarget(state, rows(), "d", 50f);
        assertNotNull(pinned);
        assertEquals("P", pinned.parentId);
        assertEquals("f1", pinned.beforeId);
    }

    @Test
    public void doesNotDropOnItselfOrNowhere() {
        SidebarState state = state();
        assertNull(SidebarUi.dropTarget(state, rows(), "c", 300f));
        // The space's header and past the last row.
        assertNull(SidebarUi.dropTarget(state, rows(), "c", 10f));
        assertNull(SidebarUi.dropTarget(state, rows(), "c", 500f));
    }

    @Test
    public void placesEveryEntryButSplitPanes() {
        SidebarState state = state();
        assertEquals("f1", SidebarUi.entryPlaces(state).get("a").getFirst());
        assertEquals("P", SidebarUi.entryPlaces(state).get("b").getFirst());
        assertEquals("T", SidebarUi.entryPlaces(state).get("d").getFirst());
    }
}
