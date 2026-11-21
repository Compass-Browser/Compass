// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONArray
import org.json.JSONObject

/**
 * What the share sheet and the join screen show (docs/design/sync.md, "Rooms: clients"), from
 * RoomsBridge's answers. The wording is the desktop's and iOS's.
 */
class ShareSheetState {
    class Member(
        @JvmField val clientId: String,
        @JvmField val name: String,
        @JvmField val email: String,
        @JvmField val role: String,
        @JvmField val pending: Boolean,
        @JvmField val me: Boolean,
    )

    class Invite(
        @JvmField val id: String,
        @JvmField val role: String,
        @JvmField val expires: Long,
        @JvmField val usesLeft: Int,
    )

    class SpaceOption(@JvmField val id: String, @JvmField val name: String, @JvmField val active: Boolean)

    /** "closed", "sheet" or "join". */
    var mode by mutableStateOf("closed")
    var busy by mutableStateOf(false)
    var error by mutableStateOf("")

    // The sheet.
    var kind by mutableStateOf("")
    var itemId by mutableStateOf("")
    var name by mutableStateOf("")
    var roomId by mutableStateOf("")
    var canShare by mutableStateOf(false)
    var role by mutableStateOf("")
    var server by mutableStateOf("")
    var oidc by mutableStateOf(false)
    var members by mutableStateOf<List<Member>>(emptyList())
    var invites by mutableStateOf<List<Invite>>(emptyList())
    var link by mutableStateOf("")
    var displayName by mutableStateOf("")

    // The join screen.
    var joinLink by mutableStateOf("")
    var joinValid by mutableStateOf(false)
    var joinKind by mutableStateOf("")
    var joinServer by mutableStateOf("")
    var alreadyJoined by mutableStateOf(false)
    var joined by mutableStateOf(false)
    var spaces by mutableStateOf<List<SpaceOption>>(emptyList())
    var joinSpace by mutableStateOf("")

    fun setItem(json: String) {
        if (json.isEmpty()) {
            name = ""
            roomId = ""
            return
        }
        val data = JSONObject(json)
        kind = data.optString("kind")
        itemId = data.optString("id")
        name = data.optString("name")
        roomId = data.optString("room_id")
        canShare = data.optBoolean("can_share")
    }

    fun setRoom(json: String) {
        val data = JSONObject(json)
        error = data.optString("error")
        val room = data.optJSONObject("room") ?: return
        roomId = room.optString("room_id")
        if (room.optString("name").isNotEmpty()) name = room.optString("name")
        role = room.optString("role")
        server = room.optString("server")
        oidc = room.optBoolean("oidc")
        val memberArray = room.optJSONArray("members") ?: JSONArray()
        members =
            (0 until memberArray.length()).mapNotNull { i ->
                memberArray.optJSONObject(i)?.let {
                    Member(
                        it.optString("client_id"),
                        it.optString("name"),
                        it.optString("email"),
                        it.optString("role"),
                        it.optBoolean("pending"),
                        it.optBoolean("is_me"),
                    )
                }
            }
        val inviteArray = room.optJSONArray("invites") ?: JSONArray()
        val now = System.currentTimeMillis()
        invites =
            (0 until inviteArray.length()).mapNotNull { i ->
                inviteArray.optJSONObject(i)?.let {
                    Invite(it.optString("id"), it.optString("role"), it.optLong("expires"), it.optInt("uses_left", -1))
                        .takeIf { invite ->
                            !it.optBoolean("revoked") && invite.usesLeft != 0 &&
                                (invite.expires == 0L || invite.expires > now)
                        }
                }
            }
    }

    fun setPreview(link: String, json: String, spacesJson: String) {
        joinLink = link
        joined = false
        joinValid = json.isNotEmpty()
        if (joinValid) {
            val data = JSONObject(json)
            name = data.optString("name")
            joinKind = data.optString("kind")
            joinServer = data.optString("server")
            alreadyJoined = data.optBoolean("already_joined")
        }
        val list = JSONArray(spacesJson.ifEmpty { "[]" })
        spaces =
            (0 until list.length()).mapNotNull { i ->
                list.optJSONObject(i)?.let {
                    SpaceOption(it.optString("id"), it.optString("name"), it.optBoolean("active"))
                }
            }
        joinSpace = spaces.firstOrNull { it.active }?.id ?: spaces.firstOrNull()?.id ?: ""
    }
}
