// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject

/** What the sync screen shows: SyncBridge's snapshot (sync_bridge.h), parsed. */
class SyncScreenState {
    class Device(
        @JvmField val guid: String,
        @JvmField val name: String,
        @JvmField val kind: String,
        @JvmField val self: Boolean,
    )

    class DataType(@JvmField val type: Int, @JvmField val enabled: Boolean)

    /** A friend on the owner's server (docs/design/sync.md, "Inviting friends"). */
    class Member(
        @JvmField val chain: String,
        @JvmField val name: String,
        @JvmField val addedMs: Long,
        /** Their profile accounts linked to it. */
        @JvmField val linked: Int,
    )

    /** An invite not used yet. */
    class Invite(@JvmField val code: String, @JvmField val name: String, @JvmField val expiresMs: Long)

    var open by mutableStateOf(false)
    /** "unavailable", "off", "needs_restart", "starting", "on", "error" or "login_required". */
    var state by mutableStateOf("off")
    var server by mutableStateOf("")
    var message by mutableStateOf("")
    /** The server removed this device from the chain on another device: only leaving is left. */
    var removed by mutableStateOf(false)
    /** A device removal is under way, and the last one's error. */
    var removing by mutableStateOf(false)
    var removeError by mutableStateOf("")
    /** "none", "working", "showing_code", "confirm", "waiting", "done" or "failed". */
    var pairingStep by mutableStateOf("none")
    var pairingCode by mutableStateOf("")
    var pairingDevice by mutableStateOf("")
    var pairingMessage by mutableStateOf("")
    var devices by mutableStateOf<List<Device>>(emptyList())
    var types by mutableStateOf<List<DataType>>(emptyList())
    var error by mutableStateOf("")
    var newWords by mutableStateOf<List<String>>(emptyList())
    /**
     * "Show recovery code", after the screen lock (SyncScreen): not in the snapshot, cleared when
     * the screen or the app is left. Never log them.
     */
    var recoveryWords by mutableStateOf<List<String>>(emptyList())
    /** The device has no screen lock, so nothing was asked. */
    var recoveryUnprotected by mutableStateOf(false)
    var recoveryError by mutableStateOf("")

    fun clearRecoveryCode() {
        recoveryWords = emptyList()
        recoveryUnprotected = false
        recoveryError = ""
    }
    // Company sign-in (docs/design/sync.md, "Company sign-in (OIDC)").
    /** The server asks chains to sign in, with this provider (may be empty). */
    var loginRequired by mutableStateOf(false)
    var loginProvider by mutableStateOf("")
    /** Signed in as. */
    var loginUserName by mutableStateOf("")
    var loginUserEmail by mutableStateOf("")
    /** A sign-in runs: the server's page shows [loginCode]. */
    var loginInProgress by mutableStateOf(false)
    var loginCode by mutableStateOf("")
    // A free server on Cloudflare (docs/design/sync.md, "A free server on Cloudflare").
    /** While one is set up or updated, what's going on ("Uploading the sync server…"). */
    var serverSetup by mutableStateOf("")
    /** "Sign in with Cloudflare" runs: its tab is open. */
    var cloudflareSignIn by mutableStateOf(false)
    // The owner's "People on your server": only on a server with an access list this chain owns.
    var peopleAvailable by mutableStateOf(false)
    var members by mutableStateOf<List<Member>>(emptyList())
    var invites by mutableStateOf<List<Invite>>(emptyList())
    /** The server has the push relay setting (version 3 on) and a relay (not off for good). */
    var pushRelayAvailable by mutableStateOf(false)
    var pushRelay by mutableStateOf(false)
    var pushRelayHost by mutableStateOf("")
    /** The switch waits for the server. */
    var peopleBusy by mutableStateOf(false)
    var peopleError by mutableStateOf("")
    /** The last invite link made, until copied or dismissed. */
    var inviteLink by mutableStateOf("")
    // "A newer version of your server is ready" (docs/design/sync.md, "Updating a server").
    var updateAvailable by mutableStateOf(false)
    var updateCurrent by mutableStateOf(0)
    var updateLatest by mutableStateOf(0)
    var updateError by mutableStateOf("")

    fun update(json: String) {
        val data = JSONObject(json)
        state = data.optString("state", "off")
        server = data.optString("server")
        message = data.optString("message")
        removed = data.optBoolean("removed")
        removing = data.optBoolean("removing")
        removeError = data.optString("remove_error")
        val pairing = data.optJSONObject("pairing")
        pairingStep = pairing?.optString("step", "none") ?: "none"
        pairingCode = pairing?.optString("code") ?: ""
        pairingDevice = pairing?.optString("device") ?: ""
        pairingMessage = pairing?.optString("message") ?: ""
        val deviceArray = data.optJSONArray("devices")
        devices =
            (0 until (deviceArray?.length() ?: 0)).mapNotNull { i ->
                deviceArray?.optJSONObject(i)?.let {
                    Device(
                        it.optString("guid"),
                        it.optString("name"),
                        it.optString("kind"),
                        it.optBoolean("this"),
                    )
                }
            }
        val typeArray = data.optJSONArray("types")
        types =
            (0 until (typeArray?.length() ?: 0)).mapNotNull { i ->
                typeArray?.optJSONObject(i)?.let { DataType(it.optInt("type"), it.optBoolean("enabled")) }
            }
        val login = data.optJSONObject("login")
        loginRequired = login?.optBoolean("required") ?: false
        loginProvider = login?.optString("provider") ?: ""
        loginUserName = login?.optString("user_name") ?: ""
        loginUserEmail = login?.optString("user_email") ?: ""
        loginInProgress = login?.optBoolean("in_progress") ?: false
        loginCode = login?.optString("code") ?: ""
        error = data.optString("error")
        val words = data.optJSONArray("new_words")
        newWords = (0 until (words?.length() ?: 0)).map { words!!.optString(it) }
        serverSetup = data.optString("server_setup")
        cloudflareSignIn = data.optBoolean("cloudflare_sign_in")
        val people = data.optJSONObject("people")
        peopleAvailable = people != null
        val memberArray = people?.optJSONArray("members")
        members =
            (0 until (memberArray?.length() ?: 0)).mapNotNull { i ->
                memberArray?.optJSONObject(i)?.let {
                    Member(
                        it.optString("chain"),
                        it.optString("name"),
                        it.optDouble("added_ms", 0.0).toLong(),
                        it.optInt("linked"),
                    )
                }
            }
        val inviteArray = people?.optJSONArray("invites")
        invites =
            (0 until (inviteArray?.length() ?: 0)).mapNotNull { i ->
                inviteArray?.optJSONObject(i)?.let {
                    Invite(it.optString("code"), it.optString("name"), it.optDouble("expires_ms", 0.0).toLong())
                }
            }
        pushRelayHost = people?.optString("push_relay_host") ?: ""
        pushRelayAvailable = (people?.optBoolean("push_relay_available") ?: false) && pushRelayHost.isNotEmpty()
        pushRelay = people?.optBoolean("push_relay") ?: false
        peopleBusy = data.optBoolean("people_busy")
        peopleError = data.optString("people_error")
        inviteLink = data.optString("invite_link")
        val update = data.optJSONObject("server_update")
        updateAvailable = update != null
        updateCurrent = update?.optInt("current") ?: 0
        updateLatest = update?.optInt("latest") ?: 0
        updateError = data.optString("update_error")
    }

    companion object {
        // What each server version brings (the desktop's sync_page.ts, NEW_IN).
        private val NEW_IN =
            mapOf(
                2 to "sharing spaces, folders and easels with others",
                3 to
                    "notifications on phones when a tab is sent to them, through " +
                        "Compass's push relay at push.compassbrowser.org",
            )

        /** The update card's note, for a server at [current] and Compass's [latest]. */
        @JvmStatic
        fun updateNote(current: Int, latest: Int): String {
            val added = (current + 1..latest).mapNotNull { NEW_IN[it] }
            return (if (added.isNotEmpty()) "It adds ${added.joinToString(", ")}. " else "") +
                "Your data and the people on it stay as they are."
        }

        /** The Phone notifications switch's details, for a server using the relay at [host]. */
        @JvmStatic
        fun pushRelayDetails(host: String): String =
            "When a tab is sent to an iPhone or Android phone, Compass's push relay ($host) " +
                "tells it at once. It gets each phone's notification token, never your tabs " +
                "or data. Off, sent tabs arrive when the phone next syncs."

        /** A friend's row: "Joined <date> · 2 linked accounts". */
        @JvmStatic
        fun memberDetails(date: String, linked: Int): String =
            "Joined $date" +
                if (linked > 0) " · $linked linked account${if (linked == 1) "" else "s"}" else ""
    }
}
