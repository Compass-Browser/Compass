// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject

/** What the Claude chat shows: ClaudeBridge's snapshot (claude_bridge.h), parsed. */
class ClaudeChatState {
    /**
     * A message: "user", "assistant", "tool" (a tool Claude used: its name) or "memory" (a change
     * Claude made to its memory: |kind| "saved", "updated" or "forgot", with Undo).
     */
    class Message(
        @JvmField val role: String,
        @JvmField val text: String,
        @JvmField val kind: String = "",
        @JvmField val undoId: Int = 0,
        @JvmField val undone: Boolean = false,
    )

    /** A fact Claude remembers (docs/design/claude.md, "Memory"). */
    class Memory(@JvmField val id: String, @JvmField val text: String)

    var open by mutableStateOf(false)
    var messages by mutableStateOf<List<Message>>(emptyList())
    var streaming by mutableStateOf(false)
    var error by mutableStateOf("")
    var hasKey by mutableStateOf(false)
    /** "Let Claude use the whole browser". */
    var browserScope by mutableStateOf(false)
    /** The model in use (compass.claude.model, or the default). */
    var model by mutableStateOf("")
    var memoryEnabled by mutableStateOf(true)
    var memorySave by mutableStateOf(true)
    /** What Claude remembers, oldest first. */
    var memories by mutableStateOf<List<Memory>>(emptyList())

    fun update(json: String) {
        val data = JSONObject(json)
        val array = data.optJSONArray("messages")
        messages =
            (0 until (array?.length() ?: 0)).mapNotNull { i ->
                array?.optJSONObject(i)?.let {
                    Message(
                        it.optString("role"),
                        it.optString("text"),
                        it.optString("kind"),
                        it.optInt("undo"),
                        it.optBoolean("undone"),
                    )
                }
            }
        streaming = data.optBoolean("streaming")
        error = data.optString("error")
        hasKey = data.optBoolean("has_key")
        browserScope = data.optBoolean("browser_scope")
        model = data.optString("model")
        memoryEnabled = data.optBoolean("memory_enabled", true)
        memorySave = data.optBoolean("memory_save", true)
        val list = data.optJSONArray("memories")
        memories =
            (0 until (list?.length() ?: 0)).mapNotNull { i ->
                list?.optJSONObject(i)?.let { Memory(it.optString("id"), it.optString("text")) }
            }
    }
}
