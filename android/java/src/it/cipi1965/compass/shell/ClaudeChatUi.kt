// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.compose.foundation.background
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner

/** The Claude chat: a sheet over the page, about the page (docs/design/claude.md). */
object ClaudeChatUi {
    interface Listener {
        fun onSend(text: String)

        fun onStop()

        /** "Let Claude use the whole browser": the tabs, not only the page. */
        fun onBrowserScope(allowed: Boolean)

        fun onClear()

        fun onSetKey(key: String)

        /** Removes the saved key, to enter another. */
        fun onForgetKey()

        /** The model the next reply uses (the model menu). */
        fun onSetModel(model: String)

        /** Takes back a change Claude made to its memory. */
        fun onUndoMemory(undoId: Int)

        /** "Use memory". */
        fun onMemoryEnabled(enabled: Boolean)

        /** "Let Claude save memories". */
        fun onMemorySave(enabled: Boolean)

        fun onAddMemory(text: String)

        fun onRemoveMemory(id: String)

        fun onRemoveAllMemories()

        fun onDismiss()
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: ClaudeChatState,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) {
                CompassSheet(state.open, onDismiss = { listener.onDismiss() }) { ClaudeSheet(state, listener) }
            }
        }
        return view
    }
}

// The models Compass offers without asking the API, as the desktop's model menu
// (resources/sidebar/claude_models.ts).
private val KNOWN_MODELS =
    listOf(
        "claude-opus-5-5" to "Claude Opus 5.5",
        "claude-sonnet-5-5" to "Claude Sonnet 5.5",
        "claude-haiku-5-5" to "Claude Haiku 5.5",
        "claude-fable-5-1" to "Claude Fable 5.1",
    )

private fun modelName(id: String): String = KNOWN_MODELS.firstOrNull { it.first == id }?.second ?: id

@Composable
private fun ClaudeSheet(state: ClaudeChatState, listener: ClaudeChatUi.Listener) {
    var memoryPage by remember { mutableStateOf(false) }
    Column(
        Modifier.fillMaxWidth().fillMaxHeight(0.7f).padding(start = 16.dp, end = 16.dp, bottom = 16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (memoryPage) {
            MemoryPage(state, listener) { memoryPage = false }
        } else {
            ChatPage(state, listener) { memoryPage = true }
        }
    }
}

/** The chat's header (the model menu, ⋯) over the key entry or the chat. */
@Composable
private fun androidx.compose.foundation.layout.ColumnScope.ChatPage(
    state: ClaudeChatState,
    listener: ClaudeChatUi.Listener,
    onMemory: () -> Unit,
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text("✦ Claude", style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
        if (state.hasKey) ModelMenu(state, listener)
        var menu by remember { mutableStateOf(false) }
        Box {
            TextButton(onClick = { menu = true }) { Text("⋯") }
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                if (state.messages.isNotEmpty()) {
                    DropdownMenuItem(text = { Text("New Chat") }, onClick = {
                        menu = false
                        listener.onClear()
                    })
                }
                DropdownMenuItem(text = { Text("Memory") }, onClick = {
                    menu = false
                    onMemory()
                })
                if (state.hasKey) {
                    DropdownMenuItem(text = { Text("Change Key") }, onClick = {
                        menu = false
                        listener.onForgetKey()
                    })
                }
            }
        }
    }
    if (!state.hasKey) KeyEntry(state, listener) else Chat(state, listener)
}

/**
 * The model menu (docs/design/claude.md: the chat's model menu): the known models, and the one in
 * use if it's another (a gateway's). Off while Claude replies; the chat goes on with the new one.
 */
@Composable
private fun ModelMenu(state: ClaudeChatState, listener: ClaudeChatUi.Listener) {
    var open by remember { mutableStateOf(false) }
    Box {
        TextButton(onClick = { open = true }, enabled = !state.streaming) {
            Text(modelName(state.model).removePrefix("Claude "), maxLines = 1)
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            val models =
                if (state.model.isEmpty() || KNOWN_MODELS.any { it.first == state.model }) KNOWN_MODELS
                else KNOWN_MODELS + (state.model to state.model)
            for ((id, name) in models) {
                DropdownMenuItem(
                    text = { Text(if (id == state.model) "✓ $name" else name) },
                    onClick = {
                        open = false
                        listener.onSetModel(id)
                    },
                )
            }
        }
    }
}

/**
 * What Claude remembers (docs/design/claude.md, "Memory"; the desktop's Claude settings, "Use
 * memory"): the two switches, the facts with × each, a field to add one, and Delete All.
 */
@Composable
private fun androidx.compose.foundation.layout.ColumnScope.MemoryPage(
    state: ClaudeChatState,
    listener: ClaudeChatUi.Listener,
    onBack: () -> Unit,
) {
    var text by remember { mutableStateOf("") }
    var deletingAll by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        TextButton(onClick = onBack) { Text("‹ Chat") }
        Text("Memory", style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text("Use memory", style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
        Switch(checked = state.memoryEnabled, onCheckedChange = { listener.onMemoryEnabled(it) })
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text("Let Claude save memories", style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
        Switch(
            checked = state.memorySave,
            enabled = state.memoryEnabled,
            onCheckedChange = { listener.onMemorySave(it) },
        )
    }
    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        if (state.memories.isEmpty()) {
            item {
                Text(
                    "Claude saves facts about you here as you chat: preferences, what you work on. You see each save, and can undo it.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        items(state.memories, key = { it.id }) { memory ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(memory.text, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
                TextButton(onClick = { listener.onRemoveMemory(memory.id) }) { Text("✕") }
            }
        }
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        @Suppress("DEPRECATION") OutlinedTextField(
            value = text,
            onValueChange = { text = it },
            placeholder = { Text("Something Claude should know") },
            modifier = Modifier.weight(1f),
            maxLines = 3,
        )
        Button(
            onClick = {
                listener.onAddMemory(text.trim())
                text = ""
            },
            enabled = text.isNotBlank(),
        ) {
            Text("Add")
        }
    }
    if (state.memories.isNotEmpty()) {
        TextButton(onClick = { deletingAll = true }) {
            Text("Delete All Memories", color = MaterialTheme.colorScheme.error)
        }
    }
    if (deletingAll) {
        AlertDialog(
            onDismissRequest = { deletingAll = false },
            title = { Text("Delete all memories?") },
            text = { Text("Claude forgets everything it saved about you, on every device that syncs.") },
            confirmButton = {
                TextButton(onClick = {
                    deletingAll = false
                    listener.onRemoveAllMemories()
                }) { Text("Delete All", color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = { TextButton(onClick = { deletingAll = false }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun KeyEntry(state: ClaudeChatState, listener: ClaudeChatUi.Listener) {
    var key by remember { mutableStateOf("") }
    Text("Claude reads the page with you. Add an Anthropic API key to start; it's kept encrypted on this device.")
    @Suppress("DEPRECATION") OutlinedTextField(
        value = key,
        onValueChange = { key = it.trim() },
        label = { Text("API key") },
        singleLine = true,
        // A password keyboard: no suggestions, no learning.
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
        modifier = Modifier.fillMaxWidth(),
    )
    if (state.error.isNotEmpty()) Text(state.error, color = MaterialTheme.colorScheme.error)
    Button(onClick = { listener.onSetKey(key) }, enabled = key.isNotEmpty()) { Text("Save") }
}

@Composable
private fun androidx.compose.foundation.layout.ColumnScope.Chat(
    state: ClaudeChatState,
    listener: ClaudeChatUi.Listener,
) {
    var text by remember { mutableStateOf("") }
    val list = rememberLazyListState()
    LaunchedEffect(state.messages.size, state.messages.lastOrNull()?.text?.length) {
        if (state.messages.isNotEmpty()) list.animateScrollToItem(state.messages.size - 1)
    }
    LazyColumn(state = list, modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (state.messages.isEmpty()) {
            item {
                Text(
                    "Ask about this page: a summary, an explanation, what it says about something.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        items(state.messages) { message ->
            if (message.role == "memory") {
                // A change to Claude's memory, with Undo (the desktop's memory line).
                Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(horizontal = 8.dp)) {
                    Text(
                        when (message.kind) {
                            "updated" -> "Updated memory: "
                            "forgot" -> "Forgot: "
                            else -> "Saved to memory: "
                        } + message.text,
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.weight(1f),
                    )
                    if (message.undone) {
                        Text(
                            "Undone",
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    } else {
                        TextButton(onClick = { listener.onUndoMemory(message.undoId) }) { Text("Undo") }
                    }
                }
                return@items
            }
            if (message.role == "tool") {
                // A tool Claude used: a quiet line between the bubbles.
                Text(
                    "· ${toolLabel(message.text)}",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 8.dp),
                )
                return@items
            }
            val mine = message.role == "user"
            Box(Modifier.fillMaxWidth(), contentAlignment = if (mine) Alignment.CenterEnd else Alignment.CenterStart) {
                Text(
                    message.text.ifEmpty { "…" },
                    modifier =
                        Modifier.background(
                                if (mine) MaterialTheme.colorScheme.primaryContainer
                                else MaterialTheme.colorScheme.surfaceContainerHighest,
                                RoundedCornerShape(16.dp),
                            )
                            .padding(12.dp),
                )
            }
        }
        if (state.error.isNotEmpty()) item { Text(state.error, color = MaterialTheme.colorScheme.error) }
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            "Let Claude use the whole browser",
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.weight(1f),
        )
        Switch(checked = state.browserScope, onCheckedChange = { listener.onBrowserScope(it) })
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        @Suppress("DEPRECATION") OutlinedTextField(
            value = text,
            onValueChange = { text = it },
            placeholder = { Text("Ask about this page") },
            modifier = Modifier.weight(1f),
            maxLines = 4,
        )
        if (state.streaming) {
            TextButton(onClick = { listener.onStop() }) { Text("Stop") }
        } else {
            Button(
                onClick = {
                    listener.onSend(text.trim())
                    text = ""
                },
                enabled = text.isNotBlank(),
            ) {
                Text("Ask")
            }
        }
    }
}

/** What a tool did, in words (common/claude/tools.json's names). */
private fun toolLabel(name: String): String =
    when (name) {
        "read_page", "get_page_text" -> "Reading the page"
        "find" -> "Looking for something on the page"
        "screenshot" -> "Looking at the page"
        "click" -> "Clicking"
        "hover" -> "Pointing"
        "type", "form_input" -> "Typing"
        "key" -> "Pressing a key"
        "scroll" -> "Scrolling"
        "navigate" -> "Going to a page"
        "wait" -> "Waiting"
        "read_console" -> "Reading the console"
        "run_js" -> "Running a script"
        "list_tabs" -> "Looking at the tabs"
        "open_tab" -> "Opening a tab"
        "select_tab" -> "Switching tabs"
        "close_tab" -> "Closing a tab"
        else -> name
    }
