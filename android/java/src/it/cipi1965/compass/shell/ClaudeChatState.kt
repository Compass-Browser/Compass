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
    class Message(@JvmField val role: String, @JvmField val text: String)

    var open by mutableStateOf(false)
    var messages by mutableStateOf<List<Message>>(emptyList())
    var streaming by mutableStateOf(false)
    var error by mutableStateOf("")
    var hasKey by mutableStateOf(false)
    /** "Let Claude use the whole browser". */
    var browserScope by mutableStateOf(false)

    fun update(json: String) {
        val data = JSONObject(json)
        val array = data.optJSONArray("messages")
        messages =
            (0 until (array?.length() ?: 0)).mapNotNull { i ->
                array?.optJSONObject(i)?.let { Message(it.optString("role"), it.optString("text")) }
            }
        streaming = data.optBoolean("streaming")
        error = data.optString("error")
        hasKey = data.optBoolean("has_key")
        browserScope = data.optBoolean("browser_scope")
    }
}
