// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.compose.foundation.text.input.TextFieldState
import androidx.compose.foundation.text.input.setTextAndPlaceCursorAtEnd
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
        /** A "tool": its input (JSON), what it answered, whether that's an error, and whether it ran. */
        @JvmField val input: String = "",
        @JvmField val result: String = "",
        @JvmField val error: Boolean = false,
        /** A "tool" ran, or a "thinking" block ended. */
        @JvmField val done: Boolean = false,
        /** A "user" message: the title of the page it carried, if one. */
        @JvmField val page: String = "",
        /** A "tool": its tool_use id, and whether it answered with a screenshot. */
        @JvmField val toolId: String = "",
        @JvmField val image: Boolean = false,
    )

    /**
     * A feature of Claude in the sidebar (docs/design/ai-sidebar.md): its key ("name_link_folders",
     * "tidy_today", "short_tab_titles", "command_bar_ask"), its switch and its model.
     */
    class Feature(@JvmField val key: String, @JvmField val on: Boolean, @JvmField val model: String)

    /** A fact Claude remembers (docs/design/claude.md, "Memory"). */
    class Memory(@JvmField val id: String, @JvmField val text: String)

    /**
     * A change waiting for the user's approval (docs/design/claude.md, "Files"): a file Claude
     * writes, a cookie it sets. |waiting|: how many more after it.
     */
    class Approval(
        @JvmField val id: Int,
        @JvmField val title: String,
        @JvmField val detail: String,
        @JvmField val alwaysAsk: Boolean,
        @JvmField val waiting: Int,
    )

    /** A Boost (docs/design/boosts.md): per-site CSS and JavaScript. */
    class Boost(
        @JvmField val id: String,
        @JvmField val name: String,
        @JvmField val match: String,
        @JvmField val enabled: Boolean,
        @JvmField val css: Boolean,
        @JvmField val js: Boolean,
    )

    var open by mutableStateOf(false)
    /** The sheet's field: kept here, as Stop puts the queue back into it. */
    val input = TextFieldState()
    /**
     * Messages written while Claude replied, oldest first: the next goes when a reply ends without
     * an error (docs/design/claude.md, "Android").
     */
    var queued by mutableStateOf<List<String>>(emptyList())
        private set
    var messages by mutableStateOf<List<Message>>(emptyList())
    var streaming by mutableStateOf(false)
    var error by mutableStateOf("")
    var hasKey by mutableStateOf(false)
    /** What the settings show of the saved key ("sk-…a1b2"), or "". */
    var keyHint by mutableStateOf("")
    /** Whether the chat can ask: a key (or a compatible server's URL) and a model. */
    var setUp by mutableStateOf(false)
    /**
     * Who answers (docs/design/ai-providers.md): "anthropic", "openai", "gemini" or "compatible";
     * the providers to pick from (id to name); the key field's placeholder, where what's sent goes,
     * and the provider's models (id to name; empty for Anthropic, whose list the sheet keeps, and a
     * compatible server).
     */
    var provider by mutableStateOf("anthropic")
    var providers by mutableStateOf<List<Pair<String, String>>>(emptyList())
    var keyPlaceholder by mutableStateOf("")
    /** The provider's server when the base URL is empty ("" for a compatible server). */
    var defaultBaseUrl by mutableStateOf("")
    var privacyNote by mutableStateOf("")
    var knownModels by mutableStateOf<List<Pair<String, String>>>(emptyList())
    /** "Let Claude use the whole browser". */
    var browserScope by mutableStateOf(false)
    /** The model in use (compass.claude.model or the provider's, or the default; "" if none is typed). */
    var model by mutableStateOf("")
    var memoryEnabled by mutableStateOf(true)
    var memorySave by mutableStateOf(true)
    /** What Claude remembers, oldest first. */
    var memories by mutableStateOf<List<Memory>>(emptyList())
    var features by mutableStateOf<List<Feature>>(emptyList())
    /** "Let Claude use developer tools". */
    var devtools by mutableStateOf(false)
    /** Files: 0 off, 1 save files (into Downloads), 2 use a folder of Downloads (|filesFolder|). */
    var filesAccess by mutableStateOf(0)
    var filesFolder by mutableStateOf("")
    var approval by mutableStateOf<Approval?>(null)
    /** "Accept the risks of subscription tokens". */
    var acceptTokenRisks by mutableStateOf(false)
    /** "Let Claude make Boosts". */
    var boostsOn by mutableStateOf(false)
    var boosts by mutableStateOf<List<Boost>>(emptyList())
    /** Whether questions carry the page (each page once a chat). */
    var attachPage by mutableStateOf(true)
    /** The last reply failed: Retry asks again. */
    var canRetry by mutableStateOf(false)
    /**
     * The provider's endpoint: base URL ("" for its own server) and, for Anthropic, how the key is
     * sent (0 x-api-key, 1 Bearer).
     */
    var baseUrl by mutableStateOf("")
    var authStyle by mutableStateOf(0)
    /** Test Connection: running, its outcome, and the models the key can use (id to name). */
    var testing by mutableStateOf(false)
    var testOk by mutableStateOf(false)
    var testResult by mutableStateOf("")
    var apiModels by mutableStateOf<List<Pair<String, String>>>(emptyList())
    /** The tab Claude acts on while it works (its Android tab ID), or -1. */
    var actingTab by mutableStateOf(-1)

    /** A list of {"id", "name"} as id to name. */
    private fun pairs(data: JSONObject, name: String): List<Pair<String, String>> {
        val list = data.optJSONArray(name)
        return (0 until (list?.length() ?: 0)).mapNotNull { i ->
            list?.optJSONObject(i)?.let { it.optString("id") to it.optString("name") }
        }
    }

    fun queue(text: String) {
        val trimmed = text.trim()
        if (trimmed.isNotEmpty()) queued = queued + trimmed
    }

    /** The next queued message, taken off the queue; null if there's none. */
    fun takeQueued(): String? = queued.firstOrNull()?.also { queued = queued.drop(1) }

    fun removeQueued(index: Int) {
        if (index in queued.indices) queued = queued.filterIndexed { i, _ -> i != index }
    }

    /** A queued message back into the field, before what's there, to change it. */
    fun editQueued(index: Int) {
        val text = queued.getOrNull(index) ?: return
        removeQueued(index)
        input.setTextAndPlaceCursorAtEnd(paragraphs(listOf(text, input.text.toString())))
    }

    /** Stop: what was queued goes back into the field, before what's there. */
    fun unqueueAll() {
        if (queued.isEmpty()) return
        input.setTextAndPlaceCursorAtEnd(paragraphs(queued + input.text.toString()))
        queued = emptyList()
    }

    fun clearQueue() {
        queued = emptyList()
    }

    private fun paragraphs(texts: List<String>) = texts.map { it.trim() }.filter { it.isNotEmpty() }.joinToString("\n\n")

    /** Reads ClaudeBridge's snapshot; true when a reply ended without an error (the queue goes on). */
    fun update(json: String): Boolean {
        val wasStreaming = streaming
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
                        it.optString("input"),
                        it.optString("result"),
                        it.optBoolean("error"),
                        it.optBoolean("done"),
                        it.optString("page"),
                        it.optString("id"),
                        it.optBoolean("image"),
                    )
                }
            }
        streaming = data.optBoolean("streaming")
        error = data.optString("error")
        hasKey = data.optBoolean("has_key")
        keyHint = data.optString("key_hint")
        setUp = data.optBoolean("set_up")
        provider = data.optString("provider", "anthropic")
        providers = pairs(data, "providers")
        keyPlaceholder = data.optString("key_placeholder")
        defaultBaseUrl = data.optString("default_base_url")
        privacyNote = data.optString("privacy_note")
        knownModels = pairs(data, "known_models")
        browserScope = data.optBoolean("browser_scope")
        model = data.optString("model")
        memoryEnabled = data.optBoolean("memory_enabled", true)
        memorySave = data.optBoolean("memory_save", true)
        val list = data.optJSONArray("memories")
        memories =
            (0 until (list?.length() ?: 0)).mapNotNull { i ->
                list?.optJSONObject(i)?.let { Memory(it.optString("id"), it.optString("text")) }
            }
        devtools = data.optBoolean("devtools")
        boostsOn = data.optBoolean("boosts_on")
        attachPage = data.optBoolean("attach_page", true)
        canRetry = data.optBoolean("can_retry")
        baseUrl = data.optString("base_url")
        authStyle = data.optInt("auth_style")
        testing = data.optBoolean("testing")
        testOk = data.optBoolean("test_ok")
        testResult = data.optString("test_result")
        apiModels = pairs(data, "api_models")
        actingTab = data.optInt("acting_tab", -1)
        acceptTokenRisks = data.optBoolean("accept_token_risks")
        val boostList = data.optJSONArray("boosts")
        boosts =
            (0 until (boostList?.length() ?: 0)).mapNotNull { i ->
                boostList?.optJSONObject(i)?.let {
                    Boost(
                        it.optString("id"),
                        it.optString("name"),
                        it.optString("match"),
                        it.optBoolean("enabled"),
                        it.optBoolean("css"),
                        it.optBoolean("js"),
                    )
                }
            }
        filesAccess = data.optInt("files_access")
        filesFolder = data.optString("files_folder")
        approval =
            data.optJSONObject("approval")?.let {
                Approval(
                    it.optInt("id"),
                    it.optString("title"),
                    it.optString("detail"),
                    it.optBoolean("always_ask"),
                    it.optInt("waiting"),
                )
            }
        val featureList = data.optJSONArray("features")
        features =
            (0 until (featureList?.length() ?: 0)).mapNotNull { i ->
                featureList?.optJSONObject(i)?.let {
                    Feature(it.optString("key"), it.optBoolean("on"), it.optString("model"))
                }
            }
        return wasStreaming && !streaming && error.isEmpty() && !canRetry
    }
}
