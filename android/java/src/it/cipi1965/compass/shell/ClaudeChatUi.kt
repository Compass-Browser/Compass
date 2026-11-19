// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.graphics.BitmapFactory
import android.util.Base64
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.annotation.DrawableRes
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.TextFieldState
import androidx.compose.foundation.text.input.clearText
import androidx.compose.foundation.text.input.rememberTextFieldState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedSecureTextField
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.SuggestionChip
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R

/**
 * The Claude chat: a Material sheet over the page, about the page (docs/design/claude.md;
 * docs/design/android.md, "Claude (C8)"). Most of the screen's lower half, or nearly all of it; the
 * conversation with Claude's thinking and tools, changes waiting for approval, and settings pages
 * from its menu.
 */
object ClaudeChatUi {
    interface Listener {
        fun onSend(text: String)

        fun onStop()

        /** "Let Claude use the whole browser": the tabs, not only the page. */
        fun onBrowserScope(allowed: Boolean)

        /** Whether questions carry the page. */
        fun onAttachPage(attach: Boolean)

        /** Asks the last question again after a failed reply. */
        fun onRetry()

        fun onClear()

        fun onSetKey(key: String)

        /** Removes the saved key, to enter another. */
        fun onForgetKey()

        /** The model the next reply uses (the model menu). */
        fun onSetModel(model: String)

        /** The endpoint: base URL ("" for the Claude API) and how the key is sent. */
        fun onSetEndpoint(baseUrl: String, authStyle: Int)

        /** Test Connection: lists the models the key can use. */
        fun onTestConnection()

        /** Takes back a change Claude made to its memory. */
        fun onUndoMemory(undoId: Int)

        /** "Use memory". */
        fun onMemoryEnabled(enabled: Boolean)

        /** "Let Claude save memories". */
        fun onMemorySave(enabled: Boolean)

        fun onAddMemory(text: String)

        /** The user's edit of a memory. */
        fun onUpdateMemory(id: String, text: String)

        fun onRemoveMemory(id: String)

        fun onRemoveAllMemories()

        /** Claude in the sidebar: a feature's switch (ClaudeChatState.Feature's key). */
        fun onSetFeature(feature: String, on: Boolean)

        /** Claude in the sidebar: a feature's model. */
        fun onSetFeatureModel(feature: String, model: String)

        /** "Accept the risks of subscription tokens". */
        fun onAcceptTokenRisks(accepted: Boolean)

        /** "Let Claude make Boosts". */
        fun onBoostsAllowed(allowed: Boolean)

        /** Takes back a change Claude made to a Boost. */
        fun onUndoBoost(undoId: Int)

        fun onBoostEnabled(id: String, enabled: Boolean)

        fun onRemoveBoost(id: String)

        /** "Let Claude use developer tools". */
        fun onDevTools(allowed: Boolean)

        /** Files: 0 off, 1 save files, 2 use |folder|, a folder of Downloads. */
        fun onFiles(access: Int, folder: String)

        /** The user's answer to an approval: 0 deny, 1 allow, 2 allow for this chat. */
        fun onAnswer(id: Int, decision: Int)

        /** A tool row's screenshot (base64 JPEG), or "". */
        fun toolImage(toolId: String): String

        /** A link in Claude's reply: opens in a new tab, as on the desktop. */
        fun onOpenLink(url: String)

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
                CompassSheet(
                    state.open,
                    onDismiss = { listener.onDismiss() },
                    containerColor = MaterialTheme.colorScheme.surfaceContainer,
                ) {
                    ClaudeSheet(state, listener)
                }
            }
        }
        return view
    }

    /**
     * "Claude is working…" with Stop, over the bar while the sheet is closed and Claude works (iOS's
     * pill); a tap opens the sheet.
     */
    @JvmStatic
    fun createWorkingPill(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        onOpen: Runnable,
        onStop: Runnable,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) {
                Box(Modifier.fillMaxWidth().padding(bottom = 10.dp), contentAlignment = Alignment.Center) {
                    Surface(
                        onClick = { onOpen.run() },
                        shape = CircleShape,
                        color = MaterialTheme.colorScheme.tertiaryContainer,
                        contentColor = MaterialTheme.colorScheme.onTertiaryContainer,
                        shadowElevation = 6.dp,
                    ) {
                        Row(
                            Modifier.padding(start = 14.dp, end = 2.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            CircularProgressIndicator(
                                Modifier.size(16.dp),
                                strokeWidth = 2.dp,
                                color = MaterialTheme.colorScheme.onTertiaryContainer,
                            )
                            Text("Claude is working…", style = MaterialTheme.typography.labelLarge)
                            IconButton(onClick = { onStop.run() }) { SymIcon(R.drawable.compass_sym_stop, "Stop") }
                        }
                    }
                }
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

private fun modelName(id: String, more: List<Pair<String, String>> = emptyList()): String =
    (KNOWN_MODELS + more).firstOrNull { it.first == id }?.second?.takeIf { it.isNotEmpty() } ?: id

/** The known models, then the endpoint's (Test Connection), then |current| if it's another. */
private fun modelChoices(current: String, apiModels: List<Pair<String, String>>): List<Pair<String, String>> {
    val list = KNOWN_MODELS.toMutableList()
    for ((id, name) in apiModels) if (list.none { it.first == id }) list.add(id to name.ifEmpty { id })
    if (current.isNotEmpty() && list.none { it.first == current }) list.add(current to current)
    return list
}

@Composable
private fun SymIcon(
    @DrawableRes id: Int,
    description: String?,
    modifier: Modifier = Modifier.size(20.dp),
    tint: Color = LocalContentColor.current,
) {
    Icon(painterResource(id), contentDescription = description, modifier = modifier, tint = tint)
}

@Composable
private fun ClaudeSheet(state: ClaudeChatState, listener: ClaudeChatUi.Listener) {
    // The page under the chat: "memory", "sidebar", "settings" or "boosts"; or none.
    var page by remember { mutableStateOf("") }
    // About half the screen (iOS's medium detent), or nearly all of it.
    var tall by remember { mutableStateOf(false) }
    val height by animateFloatAsState(if (tall || page.isNotEmpty()) 0.92f else 0.62f, label = "height")
    Column(
        Modifier.fillMaxWidth().fillMaxHeight(height).padding(start = 16.dp, end = 16.dp, bottom = 12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        when (page) {
            "memory" -> MemoryPage(state, listener) { page = "" }
            "sidebar" -> SidebarFeaturesPage(state, listener) { page = "" }
            "settings" -> SettingsPage(state, listener) { page = "" }
            "boosts" -> BoostsPage(state, listener) { page = "" }
            else -> {
                Header(state, listener, tall, onTall = { tall = !tall }, onPage = { page = it })
                if (!state.hasKey) KeyEntry(state, listener) else Chat(state, listener)
            }
        }
    }
}

/** The title, the model, New Chat, the size and the menu. */
@Composable
private fun Header(
    state: ClaudeChatState,
    listener: ClaudeChatUi.Listener,
    tall: Boolean,
    onTall: () -> Unit,
    onPage: (String) -> Unit,
) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Box(
            Modifier.size(36.dp).clip(CircleShape).background(MaterialTheme.colorScheme.primaryContainer),
            contentAlignment = Alignment.Center,
        ) {
            SymIcon(R.drawable.compass_sym_auto_awesome, null, tint = MaterialTheme.colorScheme.onPrimaryContainer)
        }
        Column(Modifier.weight(1f)) {
            Text("Claude", style = MaterialTheme.typography.titleLarge)
            if (state.hasKey) ModelMenu(state, listener)
        }
        if (state.messages.isNotEmpty()) {
            IconButton(onClick = { listener.onClear() }, enabled = !state.streaming) {
                SymIcon(R.drawable.compass_sym_edit_square, "New Chat")
            }
        }
        IconButton(onClick = onTall) {
            SymIcon(
                if (tall) R.drawable.compass_sym_expand_more else R.drawable.compass_sym_expand_less,
                if (tall) "Smaller" else "Larger",
            )
        }
        var menu by remember { mutableStateOf(false) }
        Box {
            IconButton(onClick = { menu = true }) { SymIcon(R.drawable.compass_sym_more_vert, "More") }
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                val items =
                    listOf(
                        Triple("Memory", R.drawable.compass_sym_bookmark, "memory"),
                        Triple("Boosts", R.drawable.compass_sym_bolt, "boosts"),
                        Triple("Claude in the Sidebar", R.drawable.compass_sym_view_sidebar, "sidebar"),
                        Triple("Settings", R.drawable.compass_sym_settings, "settings"),
                    )
                for ((title, icon, target) in items) {
                    DropdownMenuItem(
                        text = { Text(title) },
                        leadingIcon = { SymIcon(icon, null) },
                        onClick = {
                            menu = false
                            onPage(target)
                        },
                    )
                }
                if (state.hasKey) {
                    DropdownMenuItem(
                        text = { Text("Change Key") },
                        leadingIcon = { SymIcon(R.drawable.compass_sym_key_fill, null) },
                        onClick = {
                            menu = false
                            listener.onForgetKey()
                        },
                    )
                }
            }
        }
    }
}

/**
 * The model menu (docs/design/claude.md: the chat's model menu): the known models, the endpoint's
 * (once Test Connection listed them), and the one in use. Off while Claude replies.
 */
@Composable
private fun ModelMenu(state: ClaudeChatState, listener: ClaudeChatUi.Listener) {
    var open by remember { mutableStateOf(false) }
    Box {
        Row(
            Modifier.clip(RoundedCornerShape(8.dp)).clickable(enabled = !state.streaming) { open = true },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                modelName(state.model, state.apiModels).removePrefix("Claude "),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
                maxLines = 1,
            )
            SymIcon(R.drawable.compass_sym_expand_more, null, Modifier.size(16.dp), MaterialTheme.colorScheme.primary)
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for ((id, name) in modelChoices(state.model, state.apiModels)) {
                DropdownMenuItem(
                    text = { Text(name) },
                    trailingIcon = {
                        if (id == state.model) SymIcon(R.drawable.compass_sym_check, null, tint = MaterialTheme.colorScheme.primary)
                    },
                    onClick = {
                        open = false
                        listener.onSetModel(id)
                    },
                )
            }
        }
    }
}

/** A settings page's title row, with the way back to the chat. */
@Composable
private fun PageTitle(title: String, onBack: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        IconButton(onClick = onBack) { SymIcon(R.drawable.compass_sym_arrow_back, "Back to the chat") }
        Text(title, style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
    }
}

/** A group's heading on a settings page. */
@Composable
private fun SectionTitle(title: String) {
    Text(
        title,
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 16.dp, top = 12.dp, bottom = 4.dp),
    )
}

/** A rounded group of rows, as Material's settings lists. */
@Composable
private fun Group(content: @Composable ColumnScope.() -> Unit) {
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(20.dp)).background(MaterialTheme.colorScheme.surfaceContainerHigh),
        content = content,
    )
}

@Composable
private fun rowColors() = ListItemDefaults.colors(containerColor = Color.Transparent)

@Composable
private fun SwitchRow(title: String, about: String?, checked: Boolean, enabled: Boolean = true, onChange: (Boolean) -> Unit) {
    ListItem(
        supportingContent = about?.let { { Text(it) } },
        trailingContent = { Switch(checked = checked, enabled = enabled, onCheckedChange = onChange) },
        colors = rowColors(),
        modifier = Modifier.clickable(enabled = enabled) { onChange(!checked) },
    ) { Text(title) }
}

// Claude in the sidebar's features, as the desktop's Claude settings list them
// (claude_page.ts, SIDEBAR_FEATURES): key, title, what it does.
private val SIDEBAR_FEATURES =
    listOf(
        Triple(
            "name_link_folders",
            "Name Link Folders",
            "Folders get a short name from their tabs' titles. Suggest a Name in a folder's menu.",
        ),
        Triple("tidy_today", "Tidy Today", "Tidy in Today's header groups its tabs into named folders, with Undo."),
        Triple(
            "short_tab_titles",
            "Shorter Tab Titles",
            "Long tab titles read shorter in the sidebar. Only there: the page keeps its title.",
        ),
        Triple(
            "command_bar_ask",
            "Answer Questions in the Command Bar",
            "A question typed in the command bar gets a short answer there.",
        ),
    )

/**
 * Claude in the sidebar (docs/design/ai-sidebar.md, "Settings and privacy"): each feature's switch,
 * off by default, and its own model. Off until a key is saved.
 */
@Composable
private fun ColumnScope.SidebarFeaturesPage(state: ClaudeChatState, listener: ClaudeChatUi.Listener, onBack: () -> Unit) {
    PageTitle("Claude in the Sidebar", onBack)
    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item {
            Text(
                if (state.hasKey) "Small requests that send tab titles and hosts, never the pages themselves, and never in incognito."
                else "Add an Anthropic API key in the chat first.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp),
            )
        }
        items(SIDEBAR_FEATURES, key = { it.first }) { (key, title, about) ->
            val feature = state.features.firstOrNull { it.key == key }
            Group {
                SwitchRow(title, about, state.hasKey && feature?.on == true, state.hasKey) { listener.onSetFeature(key, it) }
                FeatureModelMenu(feature?.model ?: "", state.apiModels, enabled = state.hasKey) {
                    listener.onSetFeatureModel(key, it)
                }
            }
        }
    }
}

/** A feature's model. */
@Composable
private fun FeatureModelMenu(model: String, apiModels: List<Pair<String, String>>, enabled: Boolean, onPick: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box(Modifier.padding(start = 8.dp, bottom = 6.dp)) {
        TextButton(onClick = { open = true }, enabled = enabled) {
            Text("Model: ${modelName(model, apiModels)}", maxLines = 1)
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for ((id, name) in modelChoices(model, apiModels)) {
                DropdownMenuItem(
                    text = { Text(name) },
                    trailingIcon = { if (id == model) SymIcon(R.drawable.compass_sym_check, null) },
                    onClick = {
                        open = false
                        onPick(id)
                    },
                )
            }
        }
    }
}

/**
 * What Claude remembers (docs/design/claude.md, "Memory"): the two switches, the facts (a tap edits
 * one, × deletes it), a field to add one, and Delete All.
 */
@Composable
private fun ColumnScope.MemoryPage(state: ClaudeChatState, listener: ClaudeChatUi.Listener, onBack: () -> Unit) {
    val adding = rememberTextFieldState()
    var deletingAll by remember { mutableStateOf(false) }
    // The memory being edited (its id).
    var editing by remember { mutableStateOf("") }
    PageTitle("Memory", onBack)
    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item {
            Group {
                SwitchRow("Use memory", "What Claude remembers goes with each new chat.", state.memoryEnabled) {
                    listener.onMemoryEnabled(it)
                }
                SwitchRow("Let Claude save memories", null, state.memorySave, state.memoryEnabled) { listener.onMemorySave(it) }
            }
        }
        item { SectionTitle("What Claude remembers") }
        if (state.memories.isEmpty()) {
            item {
                Text(
                    "Claude saves facts about you here as you chat: preferences, what you work on. You see each save, and can undo it.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp),
                )
            }
        } else {
            item {
                Group {
                    for (memory in state.memories) {
                        if (editing == memory.id) {
                            // Edited in place (the desktop's settings edit a fact the same way).
                            val draft = rememberTextFieldState(memory.text)
                            val focus = remember { FocusRequester() }
                            LaunchedEffect(memory.id) { focus.requestFocus() }
                            Row(
                                Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(8.dp),
                            ) {
                                OutlinedTextField(
                                    state = draft,
                                    modifier = Modifier.weight(1f).focusRequester(focus),
                                    lineLimits = TextFieldLineLimits.MultiLine(maxHeightInLines = 4),
                                )
                                TextButton(
                                    onClick = {
                                        val text = draft.text.toString().trim()
                                        if (text.isNotEmpty() && text != memory.text) {
                                            listener.onUpdateMemory(memory.id, text)
                                        }
                                        editing = ""
                                    },
                                ) {
                                    Text("Done")
                                }
                            }
                        } else {
                            ListItem(
                                trailingContent = {
                                    IconButton(onClick = { listener.onRemoveMemory(memory.id) }) {
                                        SymIcon(R.drawable.compass_sym_close, "Delete")
                                    }
                                },
                                colors = rowColors(),
                                modifier = Modifier.clickable { editing = memory.id },
                            ) { Text(memory.text) }
                        }
                    }
                }
            }
            item {
                TextButton(onClick = { deletingAll = true }) {
                    Text("Delete All Memories", color = MaterialTheme.colorScheme.error)
                }
            }
        }
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        PillField(adding, "Something Claude should know", Modifier.weight(1f))
        FilledIconButton(
            onClick = {
                listener.onAddMemory(adding.text.toString().trim())
                adding.clearText()
            },
            enabled = adding.text.isNotBlank(),
            modifier = Modifier.size(48.dp),
        ) {
            SymIcon(R.drawable.compass_sym_add, "Add")
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

/**
 * Claude's settings (docs/design/claude.md, "Settings", "Files", "Developer tools"): the connection
 * with Test Connection, files, developer tools and subscription tokens.
 */
@Composable
private fun ColumnScope.SettingsPage(state: ClaudeChatState, listener: ClaudeChatUi.Listener, onBack: () -> Unit) {
    val folderField = rememberTextFieldState(state.filesFolder.ifEmpty { "Claude" })
    // A folder of Downloads by name: one part.
    val folder = folderField.text.toString().replace("/", "").trim()
    val urlField = rememberTextFieldState(state.baseUrl)
    val baseUrl = urlField.text.toString().trim()
    PageTitle("Settings", onBack)
    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item { SectionTitle("Connection") }
        item {
            Group {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    OutlinedTextField(
                        state = urlField,
                        label = { Text("Server") },
                        placeholder = { Text("https://api.anthropic.com") },
                        supportingText = { Text("Empty for the Claude API; else a gateway's or a compatible server's URL.") },
                        lineLimits = TextFieldLineLimits.SingleLine,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Text("Send the key as", style = MaterialTheme.typography.labelLarge)
                    SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                        val styles = listOf("x-api-key", "Bearer")
                        styles.forEachIndexed { i, label ->
                            SegmentedButton(
                                selected = state.authStyle == i,
                                onClick = { listener.onSetEndpoint(baseUrl, i) },
                                shape = SegmentedButtonDefaults.itemShape(i, styles.size),
                            ) {
                                Text(label)
                            }
                        }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                        // The saved URL is normalized (a trailing slash).
                        if (baseUrl.trimEnd('/') != state.baseUrl.trimEnd('/')) {
                            FilledTonalButton(onClick = { listener.onSetEndpoint(baseUrl, state.authStyle) }) { Text("Save") }
                        }
                        OutlinedButton(onClick = { listener.onTestConnection() }, enabled = !state.testing && state.hasKey) {
                            Text("Test Connection")
                        }
                        if (state.testing) CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                    }
                    if (state.testResult.isNotEmpty()) {
                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            SymIcon(
                                if (state.testOk) R.drawable.compass_sym_check_circle else R.drawable.compass_sym_error,
                                null,
                                tint = if (state.testOk) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
                            )
                            Text(state.testResult, style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }
        }
        item { SectionTitle("Files") }
        item {
            Group {
                val choices =
                    listOf(
                        Triple(0, "Off", "Claude can't save or read files."),
                        Triple(1, "Save Files", "Claude can save new files in Downloads."),
                        Triple(2, "Use a Folder", "Claude can list, read and write files in a folder of Downloads."),
                    )
                for ((access, title, about) in choices) {
                    ListItem(
                        supportingContent = { Text(about) },
                        leadingContent = {
                            RadioButton(
                                selected = state.filesAccess == access,
                                onClick = { listener.onFiles(access, if (access == 2) folder else "") },
                            )
                        },
                        colors = rowColors(),
                        modifier = Modifier.clickable { listener.onFiles(access, if (access == 2) folder else "") },
                    ) { Text(title) }
                }
                if (state.filesAccess == 2) {
                    Row(
                        Modifier.padding(start = 16.dp, end = 16.dp, bottom = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        OutlinedTextField(
                            state = folderField,
                            label = { Text("Folder in Downloads") },
                            lineLimits = TextFieldLineLimits.SingleLine,
                            modifier = Modifier.weight(1f),
                        )
                        FilledTonalButton(
                            onClick = { listener.onFiles(2, folder) },
                            enabled = folder.isNotEmpty() && folder != state.filesFolder,
                        ) {
                            Text("Use")
                        }
                    }
                }
            }
        }
        item {
            Text(
                "You approve each file Claude saves or writes before it happens.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp),
            )
        }
        item { SectionTitle("Advanced") }
        item {
            Group {
                SwitchRow(
                    "Let Claude use developer tools",
                    "Claude can inspect the tab it works on as a web developer would: its network requests, styles, " +
                        "storage and performance, and emulate other devices. Pages can try to give Claude " +
                        "instructions; you approve each change to cookies or storage.",
                    state.devtools,
                ) {
                    listener.onDevTools(it)
                }
                TokenRisksRow(state, listener)
            }
        }
    }
}

/**
 * "Accept the risks of subscription tokens" (docs/design/claude.md, "Settings"; the desktop's and
 * iOS's): a token from `claude setup-token` as the key, sent as Claude Code. Off by default.
 */
@Composable
private fun TokenRisksRow(state: ClaudeChatState, listener: ClaudeChatUi.Listener) {
    SwitchRow(
        "Accept the risks of subscription tokens",
        "Lets the key be a token from claude setup-token (sk-ant-oat…), which uses your Claude subscription. " +
            "Compass then sends it as Claude Code does, saying it's Claude Code. Anthropic doesn't offer these " +
            "tokens for other apps: they can stop working at any time, and using them this way may go against " +
            "the terms of your subscription. Off, such a token is refused.",
        state.acceptTokenRisks,
    ) {
        listener.onAcceptTokenRisks(it)
    }
}

/**
 * Boosts (docs/design/boosts.md): per-site CSS and JavaScript, synced with the desktop and iOS.
 * Whether Claude may make them, and each one's switch and delete.
 */
@Composable
private fun ColumnScope.BoostsPage(state: ClaudeChatState, listener: ClaudeChatUi.Listener, onBack: () -> Unit) {
    PageTitle("Boosts", onBack)
    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item {
            Group {
                SwitchRow(
                    "Let Claude make Boosts",
                    "A Boost changes how a site looks or works on all your devices. You approve every Boost with " +
                        "JavaScript; CSS-only ones save at once, with Undo.",
                    state.boostsOn,
                ) {
                    listener.onBoostsAllowed(it)
                }
            }
        }
        item { SectionTitle("Your Boosts") }
        item {
            if (state.boosts.isEmpty()) {
                Text(
                    "No Boosts yet.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp),
                )
            } else {
                Group {
                    for (boost in state.boosts) {
                        ListItem(
                            supportingContent = {
                                Text(
                                    boost.match +
                                        listOfNotNull(if (boost.css) "CSS" else null, if (boost.js) "JavaScript" else null)
                                            .joinToString(prefix = " · ", separator = ", ")
                                            .takeIf { boost.css || boost.js }
                                            .orEmpty()
                                )
                            },
                            leadingContent = { SymIcon(R.drawable.compass_sym_bolt, null) },
                            trailingContent = {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Switch(checked = boost.enabled, onCheckedChange = { listener.onBoostEnabled(boost.id, it) })
                                    IconButton(onClick = { listener.onRemoveBoost(boost.id) }) {
                                        SymIcon(R.drawable.compass_sym_delete, "Delete")
                                    }
                                }
                            },
                            colors = rowColors(),
                        ) { Text(boost.name.ifEmpty { boost.match }) }
                    }
                }
            }
        }
    }
}

/**
 * A change Claude wants to make (a file, a cookie, a Boost's JavaScript), waiting for the user:
 * Allow, For This Chat (not for a Boost's JavaScript, asked every time) and Deny.
 */
@Composable
private fun ApprovalCard(approval: ClaudeChatState.Approval, listener: ClaudeChatUi.Listener) {
    ElevatedCard(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                SymIcon(R.drawable.compass_sym_shield, null, tint = MaterialTheme.colorScheme.primary)
                Text(approval.title, style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f))
            }
            if (approval.detail.isNotEmpty()) {
                Text(
                    approval.detail,
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier =
                        Modifier.fillMaxWidth()
                            .heightIn(max = 120.dp)
                            .clip(RoundedCornerShape(12.dp))
                            .background(MaterialTheme.colorScheme.surfaceContainerHighest)
                            .verticalScroll(rememberScrollState())
                            .padding(10.dp),
                )
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { listener.onAnswer(approval.id, 1) }) { Text("Allow") }
                if (!approval.alwaysAsk) {
                    OutlinedButton(onClick = { listener.onAnswer(approval.id, 2) }) { Text("For This Chat") }
                }
                TextButton(onClick = { listener.onAnswer(approval.id, 0) }) { Text("Deny") }
            }
            if (approval.waiting > 0) {
                Text(
                    "${approval.waiting} more waiting",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/** A rounded, filled text field, as Material's search and compose fields. */
@Composable
private fun PillField(state: TextFieldState, placeholder: String, modifier: Modifier = Modifier) {
    TextField(
        state = state,
        placeholder = { Text(placeholder) },
        lineLimits = TextFieldLineLimits.MultiLine(maxHeightInLines = 5),
        shape = RoundedCornerShape(28.dp),
        colors =
            TextFieldDefaults.colors(
                focusedContainerColor = MaterialTheme.colorScheme.surfaceContainerHighest,
                unfocusedContainerColor = MaterialTheme.colorScheme.surfaceContainerHighest,
                disabledContainerColor = MaterialTheme.colorScheme.surfaceContainerHighest,
                focusedIndicatorColor = Color.Transparent,
                unfocusedIndicatorColor = Color.Transparent,
                disabledIndicatorColor = Color.Transparent,
            ),
        modifier = modifier,
    )
}

@Composable
private fun ColumnScope.KeyEntry(state: ClaudeChatState, listener: ClaudeChatUi.Listener) {
    val keyField = rememberTextFieldState()
    val key = keyField.text.toString().trim()
    Column(Modifier.weight(1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(
            "Claude reads the page with you. Add an Anthropic API key to start; it's kept encrypted on this device.",
            style = MaterialTheme.typography.bodyMedium,
        )
        // Hidden as typed, with no suggestions or learning.
        OutlinedSecureTextField(
            state = keyField,
            label = { Text("API key") },
            modifier = Modifier.fillMaxWidth(),
        )
        if (state.error.isNotEmpty()) Text(state.error, color = MaterialTheme.colorScheme.error)
        // A subscription token: the switch it needs, where it's typed.
        if (key.startsWith("sk-ant-oat")) Group { TokenRisksRow(state, listener) }
        Button(onClick = { listener.onSetKey(key) }, enabled = key.isNotEmpty()) { Text("Save") }
    }
}

// What a new chat offers to ask.
private val SUGGESTIONS = listOf("Summarize this page", "What are the key points?", "Explain it simply")

@Composable
private fun ColumnScope.Chat(state: ClaudeChatState, listener: ClaudeChatUi.Listener) {
    val input = rememberTextFieldState()
    val list = rememberLazyListState()
    LaunchedEffect(state.messages.size, state.messages.lastOrNull()?.text?.length, state.error) {
        val count = list.layoutInfo.totalItemsCount
        if (state.messages.isNotEmpty() && count > 0) list.animateScrollToItem(count - 1)
    }
    LazyColumn(state = list, modifier = Modifier.weight(1f).fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        if (state.messages.isEmpty()) {
            item {
                Column(
                    Modifier.fillMaxWidth().padding(top = 24.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Text("Ask about this page", style = MaterialTheme.typography.headlineSmall)
                    Text(
                        "A summary, an explanation, what it says about something. With the whole browser on, " +
                            "Claude can also work with your tabs and spaces.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        textAlign = TextAlign.Center,
                    )
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally)) {
                        for (suggestion in SUGGESTIONS) {
                            SuggestionChip(onClick = { listener.onSend(suggestion) }, label = { Text(suggestion) })
                        }
                    }
                }
            }
        }
        items(state.messages) { message -> MessageRow(message, listener) }
        if (state.error.isNotEmpty()) {
            item {
                Row(
                    Modifier.fillMaxWidth()
                        .clip(RoundedCornerShape(16.dp))
                        .background(MaterialTheme.colorScheme.errorContainer)
                        .padding(start = 14.dp, end = 6.dp, top = 6.dp, bottom = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    SymIcon(R.drawable.compass_sym_error, null, tint = MaterialTheme.colorScheme.onErrorContainer)
                    Text(
                        state.error,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onErrorContainer,
                        modifier = Modifier.weight(1f).padding(vertical = 6.dp),
                    )
                    if (state.canRetry) TextButton(onClick = { listener.onRetry() }) { Text("Retry") }
                }
            }
        }
    }
    state.approval?.let { ApprovalCard(it, listener) }
    // What goes with a question: the page, and the whole browser.
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        FilterChip(
            selected = state.attachPage,
            onClick = { listener.onAttachPage(!state.attachPage) },
            label = { Text("This page") },
            leadingIcon = { SymIcon(R.drawable.compass_sym_description, null, Modifier.size(FilterChipDefaults.IconSize)) },
        )
        FilterChip(
            selected = state.browserScope,
            onClick = { listener.onBrowserScope(!state.browserScope) },
            label = { Text("Whole browser") },
            leadingIcon = { SymIcon(R.drawable.compass_sym_public, null, Modifier.size(FilterChipDefaults.IconSize)) },
        )
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        PillField(input, "Ask about this page", Modifier.weight(1f))
        if (state.streaming) {
            FilledIconButton(
                onClick = { listener.onStop() },
                colors =
                    IconButtonDefaults.filledIconButtonColors(
                        containerColor = MaterialTheme.colorScheme.secondaryContainer,
                        contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
                    ),
                modifier = Modifier.size(48.dp),
            ) {
                SymIcon(R.drawable.compass_sym_stop, "Stop")
            }
        } else {
            FilledIconButton(
                onClick = {
                    listener.onSend(input.text.toString().trim())
                    input.clearText()
                },
                enabled = input.text.isNotBlank(),
                modifier = Modifier.size(48.dp),
            ) {
                SymIcon(R.drawable.compass_sym_arrow_upward, "Ask")
            }
        }
    }
}

@Composable
private fun MessageRow(message: ClaudeChatState.Message, listener: ClaudeChatUi.Listener) {
    when (message.role) {
        "user" ->
            Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(
                    message.text,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onPrimaryContainer,
                    modifier =
                        Modifier.widthIn(max = 320.dp)
                            .clip(RoundedCornerShape(20.dp, 20.dp, 6.dp, 20.dp))
                            .background(MaterialTheme.colorScheme.primaryContainer)
                            .padding(horizontal = 14.dp, vertical = 10.dp),
                )
                if (message.page.isNotEmpty()) {
                    // The page it carried, as iOS shows it.
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                        SymIcon(R.drawable.compass_sym_description, null, Modifier.size(14.dp), MaterialTheme.colorScheme.onSurfaceVariant)
                        Text(
                            message.page,
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            modifier = Modifier.widthIn(max = 260.dp),
                        )
                    }
                }
            }
        "assistant" ->
            // Claude writes Markdown (the desktop's chat renders it too); no bubble, as Material's
            // assistants show answers.
            if (message.text.isEmpty()) {
                CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
            } else {
                ClaudeMarkdown(message.text, onLink = { listener.onOpenLink(it) }, modifier = Modifier.fillMaxWidth())
            }
        "tool" -> ToolRow(message, listener)
        "thinking" -> ThinkingRow(message)
        "notice" -> Notice(message.text)
        "memory" ->
            ChangeRow(
                if (message.kind == "forgot") R.drawable.compass_sym_remove else R.drawable.compass_sym_bookmark,
                when (message.kind) {
                    "updated" -> "Updated memory: "
                    "forgot" -> "Forgot: "
                    else -> "Saved to memory: "
                } + message.text,
                message.undone,
            ) {
                listener.onUndoMemory(message.undoId)
            }
        "boost" ->
            ChangeRow(
                if (message.kind == "deleted") R.drawable.compass_sym_delete else R.drawable.compass_sym_bolt,
                when (message.kind) {
                    "updated" -> "Updated Boost for "
                    "deleted" -> "Deleted Boost for "
                    else -> "Saved Boost for "
                } + message.text,
                message.undone,
            ) {
                listener.onUndoBoost(message.undoId)
            }
    }
}

/** Why a reply ended early: declined, or cut off. */
@Composable
private fun Notice(text: String) {
    Row(
        Modifier.fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(MaterialTheme.colorScheme.tertiaryContainer)
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        SymIcon(R.drawable.compass_sym_info, null, tint = MaterialTheme.colorScheme.onTertiaryContainer)
        Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onTertiaryContainer)
    }
}

/** A change Claude made to its memory or a Boost, with Undo. */
@Composable
private fun ChangeRow(@DrawableRes icon: Int, text: String, undone: Boolean, onUndo: () -> Unit) {
    Row(
        Modifier.fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(MaterialTheme.colorScheme.secondaryContainer)
            .padding(start = 12.dp, end = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        SymIcon(icon, null, tint = MaterialTheme.colorScheme.onSecondaryContainer)
        Text(
            text,
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSecondaryContainer,
            modifier = Modifier.weight(1f).padding(vertical = 10.dp),
        )
        if (undone) {
            Text(
                "Undone",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSecondaryContainer,
                modifier = Modifier.padding(end = 12.dp),
            )
        } else {
            TextButton(onClick = onUndo) { Text("Undo") }
        }
    }
}

/**
 * A tool Claude used, as the desktop's chat shows it: what it did and to what, running, done or
 * failed; a tap opens its input, what it answered and its screenshot.
 */
@Composable
private fun ToolRow(message: ClaudeChatState.Message, listener: ClaudeChatUi.Listener) {
    var open by remember { mutableStateOf(false) }
    val muted = MaterialTheme.colorScheme.onSurfaceVariant
    Column(
        Modifier.fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(MaterialTheme.colorScheme.surfaceContainerHigh)
            .animateContentSize()
    ) {
        Row(
            Modifier.fillMaxWidth().clickable { open = !open }.padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            SymIcon(R.drawable.compass_sym_build, null, Modifier.size(16.dp), muted)
            Text(
                toolLabel(message.text) + toolSubject(message.input),
                style = MaterialTheme.typography.labelMedium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
            when {
                !message.done -> CircularProgressIndicator(Modifier.size(14.dp), strokeWidth = 2.dp)
                message.error -> SymIcon(R.drawable.compass_sym_error, "Failed", Modifier.size(16.dp), MaterialTheme.colorScheme.error)
                else -> SymIcon(R.drawable.compass_sym_check_circle, "Done", Modifier.size(16.dp), MaterialTheme.colorScheme.primary)
            }
            SymIcon(R.drawable.compass_sym_expand_more, null, Modifier.size(16.dp).rotate(if (open) 180f else 0f), muted)
        }
        if (open) {
            Column(
                Modifier.fillMaxWidth().padding(start = 12.dp, end = 12.dp, bottom = 10.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Text(message.text, style = MaterialTheme.typography.labelSmall, color = muted)
                if (message.input.isNotEmpty() && message.input != "{}") {
                    Text(prettyJson(message.input), style = MaterialTheme.typography.labelSmall, fontFamily = FontFamily.Monospace)
                }
                if (message.image) {
                    val bitmap =
                        remember(message.toolId) {
                            runCatching {
                                    val bytes = Base64.decode(listener.toolImage(message.toolId), Base64.DEFAULT)
                                    BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.asImageBitmap()
                                }
                                .getOrNull()
                        }
                    if (bitmap != null) {
                        Image(
                            bitmap,
                            contentDescription = "Screenshot",
                            contentScale = ContentScale.FillWidth,
                            modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)),
                        )
                    }
                }
                if (message.result.isNotEmpty()) {
                    HorizontalDivider()
                    Text(
                        message.result,
                        style = MaterialTheme.typography.labelSmall,
                        fontFamily = FontFamily.Monospace,
                        color = if (message.error) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface,
                        maxLines = 40,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }
    }
}

/**
 * Claude's thinking, summarized, as the desktop's "Thinking…" row: "Thinking…" while it streams,
 * "Thought" after; a tap shows the summary.
 */
@Composable
private fun ThinkingRow(message: ClaudeChatState.Message) {
    var open by remember { mutableStateOf(false) }
    val muted = MaterialTheme.colorScheme.onSurfaceVariant
    Column(Modifier.fillMaxWidth().animateContentSize()) {
        Row(
            Modifier.clip(RoundedCornerShape(10.dp)).clickable { open = !open }.padding(4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            SymIcon(R.drawable.compass_sym_psychology, null, Modifier.size(16.dp), muted)
            Text(if (message.done) "Thought" else "Thinking…", style = MaterialTheme.typography.labelMedium, color = muted)
            if (!message.done) CircularProgressIndicator(Modifier.size(12.dp), strokeWidth = 1.5.dp)
            SymIcon(R.drawable.compass_sym_expand_more, null, Modifier.size(16.dp).rotate(if (open) 180f else 0f), muted)
        }
        if (open && message.text.isNotBlank()) {
            Text(
                message.text.trim(),
                style = MaterialTheme.typography.bodySmall,
                fontStyle = FontStyle.Italic,
                color = muted,
                modifier = Modifier.padding(start = 28.dp, top = 2.dp),
            )
        }
    }
}

/** A few words on what a tool acted on: its URL, ref, text or query, from its input. */
private fun toolSubject(input: String): String {
    val json = runCatching { org.json.JSONObject(input) }.getOrNull() ?: return ""
    for (key in listOf("url", "query", "text", "ref", "tab_id", "path", "name", "selector", "key", "space")) {
        val value = json.optString(key)
        if (value.isNotEmpty()) return ": " + if (value.length > 40) value.take(40) + "…" else value
    }
    return ""
}

private fun prettyJson(input: String): String = runCatching { org.json.JSONObject(input).toString(2) }.getOrDefault(input)

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
        "activate_tab" -> "Showing a tab"
        "move_tab" -> "Moving a tab"
        "rename_tab" -> "Renaming"
        "create_folder" -> "Making a folder"
        "mute_tab" -> "Muting a tab"
        "split_tabs" -> "Putting tabs side by side"
        "switch_space" -> "Switching spaces"
        "create_space" -> "Making a space"
        "search_history" -> "Searching history"
        "list_downloads" -> "Looking at downloads"
        "send_tab_to_device" -> "Sending a tab"
        "network_requests" -> "Looking at network requests"
        "network_response" -> "Reading a response"
        "inspect_element" -> "Inspecting an element"
        "page_storage" -> "Reading the page's storage"
        "edit_page_storage" -> "Changing the page's storage"
        "performance" -> "Measuring performance"
        "emulate" -> "Emulating a device"
        "save_file" -> "Saving a file"
        "list_files" -> "Listing files"
        "read_file" -> "Reading a file"
        "write_file" -> "Writing a file"
        "list_boosts" -> "Looking at Boosts"
        "save_boost" -> "Saving a Boost"
        "delete_boost" -> "Deleting a Boost"
        else -> name
    }
