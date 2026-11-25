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

/**
 * The sync screen's state from SyncBridge's snapshot (sync_bridge.cc): a server being set up on
 * Cloudflare, and an owner's people, phone notifications and server update.
 */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE)
public class SyncScreenStateTest {
    @Test
    public void testServerSetup() {
        SyncScreenState state = new SyncScreenState();
        state.update(
                "{\"state\": \"off\", \"server_setup\": \"Uploading the sync server…\","
                        + " \"cloudflare_sign_in\": false}");
        assertEquals("off", state.getState());
        assertEquals("Uploading the sync server…", state.getServerSetup());
        assertFalse(state.getCloudflareSignIn());
        assertFalse(state.getPeopleAvailable());
        assertFalse(state.getUpdateAvailable());

        state.update(
                "{\"state\": \"off\", \"server_setup\": \"Sign in to Cloudflare in the new tab"
                        + " and allow Compass…\", \"cloudflare_sign_in\": true}");
        assertTrue(state.getCloudflareSignIn());

        // Older natives (and a profile without sync) say nothing of it.
        state.update("{\"state\": \"unavailable\"}");
        assertEquals("", state.getServerSetup());
        assertFalse(state.getCloudflareSignIn());
    }

    @Test
    public void testPeople() {
        SyncScreenState state = new SyncScreenState();
        state.update(
                "{\"state\": \"on\", \"people\": {"
                        + " \"members\": [{\"chain\": \"ab12\", \"name\": \"Anna\","
                        + "  \"added_ms\": 1700000000000.0, \"linked\": 2},"
                        + "  {\"chain\": \"cd34\", \"name\": \"\", \"added_ms\": 1600000000000.0,"
                        + "  \"linked\": 0}],"
                        + " \"invites\": [{\"code\": \"k7\", \"name\": \"Bo\","
                        + "  \"expires_ms\": 1800000000000.0}],"
                        + " \"push_relay_available\": true, \"push_relay\": true,"
                        + " \"push_relay_host\": \"push.compassbrowser.org\"},"
                        + " \"people_error\": \"Only the server's owners can do that.\","
                        + " \"invite_link\": \"https://s.example/invite#k7\","
                        + " \"people_busy\": true}");
        assertTrue(state.getPeopleAvailable());
        assertEquals(2, state.getMembers().size());
        assertEquals("ab12", state.getMembers().get(0).chain);
        assertEquals("Anna", state.getMembers().get(0).name);
        assertEquals(1700000000000L, state.getMembers().get(0).addedMs);
        assertEquals(2, state.getMembers().get(0).linked);
        assertEquals("", state.getMembers().get(1).name);
        assertEquals(1, state.getInvites().size());
        assertEquals("k7", state.getInvites().get(0).code);
        assertEquals(1800000000000L, state.getInvites().get(0).expiresMs);
        assertTrue(state.getPushRelayAvailable());
        assertTrue(state.getPushRelay());
        assertEquals("push.compassbrowser.org", state.getPushRelayHost());
        assertTrue(state.getPeopleBusy());
        assertEquals("Only the server's owners can do that.", state.getPeopleError());
        assertEquals("https://s.example/invite#k7", state.getInviteLink());

        // Not an owner (or sync went off): no people.
        state.update("{\"state\": \"on\"}");
        assertFalse(state.getPeopleAvailable());
        assertTrue(state.getMembers().isEmpty());
        assertTrue(state.getInvites().isEmpty());
        assertFalse(state.getPushRelayAvailable());
        assertEquals("", state.getInviteLink());
    }

    @Test
    public void testPushRelaySwitchNeedsARelay() {
        SyncScreenState state = new SyncScreenState();
        // Push off for good on the server: no relay, so no switch.
        state.update(
                "{\"state\": \"on\", \"people\": {\"members\": [], \"invites\": [],"
                        + " \"push_relay_available\": true, \"push_relay\": false,"
                        + " \"push_relay_host\": \"\"}}");
        assertTrue(state.getPeopleAvailable());
        assertFalse(state.getPushRelayAvailable());
        // A server from before version 3: no setting.
        state.update(
                "{\"state\": \"on\", \"people\": {\"members\": [], \"invites\": [],"
                        + " \"push_relay_available\": false, \"push_relay\": false,"
                        + " \"push_relay_host\": \"push.compassbrowser.org\"}}");
        assertFalse(state.getPushRelayAvailable());
    }

    @Test
    public void testServerUpdate() {
        SyncScreenState state = new SyncScreenState();
        state.update(
                "{\"state\": \"on\", \"server_update\": {\"current\": 1, \"latest\": 3},"
                        + " \"update_error\": \"Cloudflare refused the token.\"}");
        assertTrue(state.getUpdateAvailable());
        assertEquals(1, state.getUpdateCurrent());
        assertEquals(3, state.getUpdateLatest());
        assertEquals("Cloudflare refused the token.", state.getUpdateError());
        state.update("{\"state\": \"on\"}");
        assertFalse(state.getUpdateAvailable());
        assertEquals("", state.getUpdateError());
    }

    @Test
    public void testUpdateNote() {
        assertEquals(
                "It adds sharing spaces, folders and easels with others, notifications on phones"
                        + " when a tab is sent to them, through Compass's push relay at"
                        + " push.compassbrowser.org. Your data and the people on it stay as they"
                        + " are.",
                SyncScreenState.updateNote(1, 3));
        assertEquals(
                "It adds notifications on phones when a tab is sent to them, through Compass's"
                        + " push relay at push.compassbrowser.org. Your data and the people on it"
                        + " stay as they are.",
                SyncScreenState.updateNote(2, 3));
        // A version with nothing to tell.
        assertEquals(
                "Your data and the people on it stay as they are.",
                SyncScreenState.updateNote(3, 4));
    }

    @Test
    public void testWording() {
        assertEquals("Joined May 1, 2026", SyncScreenState.memberDetails("May 1, 2026", 0));
        assertEquals(
                "Joined May 1, 2026 · 1 linked account",
                SyncScreenState.memberDetails("May 1, 2026", 1));
        assertEquals(
                "Joined May 1, 2026 · 3 linked accounts",
                SyncScreenState.memberDetails("May 1, 2026", 3));
        assertTrue(
                SyncScreenState.pushRelayDetails("relay.example")
                        .startsWith(
                                "When a tab is sent to an iPhone or Android phone, Compass's push"
                                        + " relay (relay.example) tells it at once."));
    }
}
