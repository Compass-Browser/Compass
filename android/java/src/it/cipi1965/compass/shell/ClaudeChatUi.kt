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
import androidx.compose.material3.Button
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

@Composable
private fun ClaudeSheet(state: ClaudeChatState, listener: ClaudeChatUi.Listener) {
    Column(
        Modifier.fillMaxWidth().fillMaxHeight(0.7f).padding(start = 16.dp, end = 16.dp, bottom = 16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("✦ Claude", style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
            if (state.messages.isNotEmpty()) TextButton(onClick = { listener.onClear() }) { Text("New chat") }
            if (state.hasKey) TextButton(onClick = { listener.onForgetKey() }) { Text("Change key") }
        }
        if (!state.hasKey) KeyEntry(state, listener) else Chat(state, listener)
    }
}

@Composable
private fun KeyEntry(state: ClaudeChatState, listener: ClaudeChatUi.Listener) {
    var key by remember { mutableStateOf("") }
    Text("Claude reads the page with you. Add an Anthropic API key to start; it's kept encrypted on this device.")
    OutlinedTextField(
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
        OutlinedTextField(
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
