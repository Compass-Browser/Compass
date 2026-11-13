// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.annotation.Config;

import org.chromium.base.test.BaseRobolectricTestRunner;

/** The Claude sheet's state from ClaudeBridge's snapshot: memory lines, the memories, the model. */
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
}
