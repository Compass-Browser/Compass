// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.annotation.Config;

import org.chromium.base.test.BaseRobolectricTestRunner;

/**
 * The Claude sheet's state from ClaudeBridge's snapshot: memory lines, the memories, the model;
 * and the queue of messages written while Claude replies.
 */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE)
public class ClaudeChatStateTest {
    @Test
    public void readsMemoryAndTheModel() {
        ClaudeChatState state = new ClaudeChatState();
        state.update(
                "{\"messages\": ["
                        + " {\"role\": \"user\", \"text\": \"I use metric\"},"
                        + " {\"role\": \"memory\", \"text\": \"Prefers metric units\","
                        + "  \"kind\": \"saved\", \"undo\": 3, \"undone\": false},"
                        + " {\"role\": \"memory\", \"text\": \"Lives in Milan\","
                        + "  \"kind\": \"forgot\", \"undo\": 4, \"undone\": true}],"
                        + " \"streaming\": false, \"error\": \"\", \"has_key\": true,"
                        + " \"browser_scope\": false, \"model\": \"claude-sonnet-5-5\","
                        + " \"memory_enabled\": true, \"memory_save\": false,"
                        + " \"memories\": [{\"id\": \"0a1b\", \"text\": \"Prefers metric units\"}]}");
        assertEquals(3, state.getMessages().size());
        ClaudeChatState.Message saved = state.getMessages().get(1);
        assertEquals("memory", saved.role);
        assertEquals("saved", saved.kind);
        assertEquals(3, saved.undoId);
        assertFalse(saved.undone);
        assertTrue(state.getMessages().get(2).undone);
        assertEquals("claude-sonnet-5-5", state.getModel());
        assertTrue(state.getMemoryEnabled());
        assertFalse(state.getMemorySave());
        assertEquals(1, state.getMemories().size());
        assertEquals("0a1b", state.getMemories().get(0).id);
    }

    @Test
    public void memoryIsOnByDefault() {
        ClaudeChatState state = new ClaudeChatState();
        state.update("{\"messages\": []}");
        assertTrue(state.getMemoryEnabled());
        assertTrue(state.getMemorySave());
        assertTrue(state.getMemories().isEmpty());
    }

    private static String snapshot(boolean streaming, String error, boolean canRetry) {
        return "{\"messages\": [], \"streaming\": " + streaming + ", \"error\": \"" + error
                + "\", \"can_retry\": " + canRetry + "}";
    }

    @Test
    public void updateSaysWhenAReplyEndedWell() {
        ClaudeChatState state = new ClaudeChatState();
        assertFalse(state.update(snapshot(false, "", false)));
        assertFalse(state.update(snapshot(true, "", false)));
        assertFalse(state.update(snapshot(true, "", false)));
        assertTrue(state.update(snapshot(false, "", false)));
        // An error: the queue waits.
        state.update(snapshot(true, "", false));
        assertFalse(state.update(snapshot(false, "Overloaded", true)));
    }

    @Test
    public void queuesInOrder() {
        ClaudeChatState state = new ClaudeChatState();
        state.queue("  First  ");
        state.queue(" ");
        state.queue("Second");
        assertEquals(2, state.getQueued().size());
        assertEquals("First", state.takeQueued());
        assertEquals("Second", state.takeQueued());
        assertNull(state.takeQueued());
    }

    @Test
    public void editAndStopPutTheQueueBackInTheField() {
        ClaudeChatState state = new ClaudeChatState();
        state.queue("One");
        state.queue("Two");
        state.queue("Three");
        state.editQueued(1);
        assertEquals("Two", state.getInput().getText().toString());
        state.removeQueued(0);
        // Before what's in the field.
        state.unqueueAll();
        assertEquals("Three\n\nTwo", state.getInput().getText().toString());
        assertTrue(state.getQueued().isEmpty());
    }
}
