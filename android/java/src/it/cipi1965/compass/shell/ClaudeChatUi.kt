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
import androidx.annotation.StringRes
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
import androidx.compose.runtime.key
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
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
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
        /** While Claude replies, queues the text; empty between replies, sends the next queued. */
        fun onSend(text: String)

        fun onStop()

        /** A queued message back into the field. */
        fun onEditQueued(index: Int)

        fun onRemoveQueued(index: Int)

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

        /**
         * Who answers: "anthropic", "openai", "gemini" or "compatible" (docs/design/ai-providers.md).
         */
        fun onSetProvider(provider: String)

        /** Sign in with ChatGPT (OpenAI): opens OpenAI's sign-in in a tab. */
        fun onSignInWithChatGpt()

        fun onSignOutOfChatGpt()

        /** The provider's endpoint: base URL ("" for its own server) and how the key is sent. */
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
                            Text(stringResource(R.string.compass_claude_working), style = MaterialTheme.typography.labelLarge)
                            IconButton(onClick = { onStop.run() }) { SymIcon(R.drawable.compass_sym_stop, stringResource(R.string.compass_claude_stop)) }
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

/** The provider's known models: Anthropic's from the list above, another's from the bridge. */
private val ClaudeChatState.known: List<Pair<String, String>>
    get() = if (provider == "anthropic") KNOWN_MODELS else knownModels

/** The provider's name, as the settings' picker shows it. */
private val ClaudeChatState.providerName: String
    get() = providers.firstOrNull { it.first == provider }?.second ?: "Anthropic"

private fun modelName(id: String, models: List<Pair<String, String>>, sameAsChat: String): String =
    if (id.isEmpty()) sameAsChat
    else models.firstOrNull { it.first == id }?.second?.takeIf { it.isNotEmpty() } ?: id

/**
 * The provider's known models, then the endpoint's (Test Connection), then |current| if it's
 * another; "Same as chat" first where empty means the chat's model (a sidebar feature with a
 * compatible server).
 */
private fun modelChoices(
    state: ClaudeChatState,
    current: String,
    sameAsChat: Boolean = false,
    sameAsChatLabel: String = "",
): List<Pair<String, String>> {
    val list = state.known.toMutableList()
    for ((id, name) in state.apiModels) if (list.none { it.first == id }) list.add(id to name.ifEmpty { id })
    if (current.isNotEmpty() && list.none { it.first == current }) list.add(current to current)
    if (sameAsChat) list.add(0, "" to sameAsChatLabel)
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
                if (!state.setUp) KeyEntry(state, listener) { page = "settings" } else Chat(state, listener)
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
            if (state.setUp) ModelMenu(state, listener)
        }
        if (state.messages.isNotEmpty()) {
            IconButton(onClick = { listener.onClear() }, enabled = !state.streaming) {
                SymIcon(R.drawable.compass_sym_edit_square, stringResource(R.string.compass_claude_new_chat))
            }
        }
        IconButton(onClick = onTall) {
            SymIcon(
                if (tall) R.drawable.compass_sym_expand_more else R.drawable.compass_sym_expand_less,
                stringResource(if (tall) R.string.compass_claude_smaller else R.string.compass_claude_larger),
            )
        }
        var menu by remember { mutableStateOf(false) }
        Box {
            IconButton(onClick = { menu = true }) { SymIcon(R.drawable.compass_sym_more_vert, stringResource(R.string.compass_more)) }
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                val items =
                    listOf(
                        Triple(R.string.compass_claude_memory, R.drawable.compass_sym_bookmark, "memory"),
                        Triple(R.string.compass_claude_boosts, R.drawable.compass_sym_bolt, "boosts"),
                        Triple(R.string.compass_claude_in_sidebar, R.drawable.compass_sym_view_sidebar, "sidebar"),
                        Triple(R.string.compass_settings, R.drawable.compass_sym_settings, "settings"),
                    )
                for ((title, icon, target) in items) {
                    DropdownMenuItem(
                        text = { Text(stringResource(title)) },
                        leadingIcon = { SymIcon(icon, null) },
                        onClick = {
                            menu = false
                            onPage(target)
                        },
                    )
                }
                if (state.hasKey) {
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.compass_claude_change_key)) },
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
 * The model menu (docs/design/claude.md: the chat's model menu): the provider's known models, the endpoint's
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
                modelName(state.model, state.known + state.apiModels, stringResource(R.string.compass_claude_same_as_chat)).removePrefix("Claude "),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
                maxLines = 1,
            )
            SymIcon(R.drawable.compass_sym_expand_more, null, Modifier.size(16.dp), MaterialTheme.colorScheme.primary)
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for ((id, name) in modelChoices(state, state.model)) {
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
        IconButton(onClick = onBack) { SymIcon(R.drawable.compass_sym_arrow_back, stringResource(R.string.compass_claude_back_to_chat)) }
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
            R.string.compass_claude_feature_name_link_folders,
            R.string.compass_claude_feature_name_link_folders_about,
        ),
        Triple("tidy_today", R.string.compass_claude_feature_tidy_today, R.string.compass_claude_feature_tidy_today_about),
        Triple(
            "short_tab_titles",
            R.string.compass_claude_feature_short_tab_titles,
            R.string.compass_claude_feature_short_tab_titles_about,
        ),
        Triple(
            "command_bar_ask",
            R.string.compass_claude_feature_command_bar_ask,
            R.string.compass_claude_feature_command_bar_ask_about,
        ),
    )

/**
 * Claude in the sidebar (docs/design/ai-sidebar.md, "Settings and privacy"): each feature's switch,
 * off by default, and its own model. Off until a key is saved.
 */
@Composable
private fun ColumnScope.SidebarFeaturesPage(state: ClaudeChatState, listener: ClaudeChatUi.Listener, onBack: () -> Unit) {
    PageTitle(stringResource(R.string.compass_claude_in_sidebar), onBack)
    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item {
            Text(
                if (state.setUp) {
                    stringResource(R.string.compass_claude_sidebar_privacy, state.providerName)
                } else {
                    stringResource(R.string.compass_claude_sidebar_set_up_first, state.providerName)
                },
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp),
            )
        }
        items(SIDEBAR_FEATURES, key = { it.first }) { (key, title, about) ->
            val feature = state.features.firstOrNull { it.key == key }
            Group {
                val model = feature?.model ?: ""
                SwitchRow(stringResource(title), stringResource(about), state.setUp && feature?.on == true, state.setUp) {
                    listener.onSetFeature(key, it)
                }
                // A compatible server's features can ask the chat's model (empty).
                val choices =
                    modelChoices(
                        state,
                        model,
                        sameAsChat = state.provider == "compatible",
                        sameAsChatLabel = stringResource(R.string.compass_claude_same_as_chat),
                    )
                ModelMenuButton(model, choices, state.setUp) {
                    listener.onSetFeatureModel(key, it)
                }
            }
        }
    }
}

/** A model picker in a settings group (a sidebar feature's, the chat's): |choices| is id to name. */
@Composable
private fun ModelMenuButton(model: String, choices: List<Pair<String, String>>, enabled: Boolean, onPick: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box(Modifier.padding(start = 8.dp, bottom = 6.dp)) {
        TextButton(onClick = { open = true }, enabled = enabled) {
            Text(stringResource(R.string.compass_claude_model_label, modelName(model, choices, stringResource(R.string.compass_claude_same_as_chat))), maxLines = 1)
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for ((id, name) in choices) {
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
    PageTitle(stringResource(R.string.compass_claude_memory), onBack)
    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item {
            Group {
                SwitchRow(stringResource(R.string.compass_claude_use_memory), stringResource(R.string.compass_claude_use_memory_about), state.memoryEnabled) {
                    listener.onMemoryEnabled(it)
                }
                SwitchRow(stringResource(R.string.compass_claude_save_memories), null, state.memorySave, state.memoryEnabled) { listener.onMemorySave(it) }
            }
        }
        item { SectionTitle(stringResource(R.string.compass_claude_what_claude_remembers)) }
        if (state.memories.isEmpty()) {
            item {
                Text(
                    stringResource(R.string.compass_claude_memories_empty),
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
                                    Text(stringResource(R.string.compass_done))
                                }
                            }
                        } else {
                            ListItem(
                                trailingContent = {
                                    IconButton(onClick = { listener.onRemoveMemory(memory.id) }) {
                                        SymIcon(R.drawable.compass_sym_close, stringResource(R.string.compass_delete))
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
                    Text(stringResource(R.string.compass_claude_delete_all_memories), color = MaterialTheme.colorScheme.error)
                }
            }
        }
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        PillField(adding, stringResource(R.string.compass_claude_memory_placeholder), Modifier.weight(1f))
        FilledIconButton(
            onClick = {
                listener.onAddMemory(adding.text.toString().trim())
                adding.clearText()
            },
            enabled = adding.text.isNotBlank(),
            modifier = Modifier.size(48.dp),
        ) {
            SymIcon(R.drawable.compass_sym_add, stringResource(R.string.compass_add))
        }
    }
    if (deletingAll) {
        AlertDialog(
            onDismissRequest = { deletingAll = false },
            title = { Text(stringResource(R.string.compass_claude_delete_all_memories_title)) },
            text = { Text(stringResource(R.string.compass_claude_delete_all_memories_text)) },
            confirmButton = {
                TextButton(onClick = {
                    deletingAll = false
                    listener.onRemoveAllMemories()
                }) { Text(stringResource(R.string.compass_claude_delete_all), color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = { TextButton(onClick = { deletingAll = false }) { Text(stringResource(R.string.compass_cancel)) } },
        )
    }
}

/**
 * Claude's settings (docs/design/claude.md, "Settings", "Files", "Developer tools"): the provider
 * (docs/design/ai-providers.md, "Settings") with what's sent where, its key, its server and model
 * with Test Connection, files, developer tools and subscription tokens (Anthropic's only).
 */
@Composable
private fun ColumnScope.SettingsPage(state: ClaudeChatState, listener: ClaudeChatUi.Listener, onBack: () -> Unit) {
    val folderField = rememberTextFieldState(state.filesFolder.ifEmpty { "Claude" })
    // A folder of Downloads by name: one part.
    val folder = folderField.text.toString().replace("/", "").trim()
    // Each provider has its own key, server and model: the fields start over with another.
    val urlField = key(state.provider) { rememberTextFieldState(state.baseUrl) }
    val baseUrl = urlField.text.toString().trim()
    val modelField = key(state.provider) { rememberTextFieldState(state.model) }
    val modelId = modelField.text.toString().trim()
    val keyField = key(state.provider) { rememberTextFieldState() }
    val apiKey = keyField.text.toString().trim()
    val anthropic = state.provider == "anthropic"
    val compatible = state.provider == "compatible"
    PageTitle(stringResource(R.string.compass_settings), onBack)
    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item { SectionTitle(stringResource(R.string.compass_claude_provider)) }
        item {
            Group {
                for ((id, name) in state.providers) {
                    ListItem(
                        leadingContent = {
                            RadioButton(
                                selected = state.provider == id,
                                onClick = { listener.onSetProvider(id) },
                                enabled = !state.streaming,
                            )
                        },
                        colors = rowColors(),
                        modifier = Modifier.clickable(enabled = !state.streaming) { listener.onSetProvider(id) },
                    ) { Text(name) }
                }
            }
        }
        if (state.privacyNote.isNotEmpty()) {
            item {
                Text(
                    state.privacyNote,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp),
                )
            }
        }
        if (compatible) {
            // An OpenAI-compatible server: its URL and the model to ask, both needed.
            item { SectionTitle(stringResource(R.string.compass_claude_server)) }
            item {
                Group {
                    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        OutlinedTextField(
                            state = urlField,
                            label = { Text(stringResource(R.string.compass_claude_server_url)) },
                            placeholder = { Text("https://example.com/v1") },
                            supportingText = {
                                Text(stringResource(R.string.compass_claude_server_url_about))
                            },
                            lineLimits = TextFieldLineLimits.SingleLine,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                            modifier = Modifier.fillMaxWidth(),
                        )
                        OutlinedTextField(
                            state = modelField,
                            label = { Text(stringResource(R.string.compass_claude_model_id)) },
                            lineLimits = TextFieldLineLimits.SingleLine,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                            modifier = Modifier.fillMaxWidth(),
                        )
                        if (baseUrl.trimEnd('/') != state.baseUrl.trimEnd('/') || modelId != state.model) {
                            FilledTonalButton(
                                onClick = {
                                    if (baseUrl.trimEnd('/') != state.baseUrl.trimEnd('/')) {
                                        listener.onSetEndpoint(baseUrl, state.authStyle)
                                    }
                                    if (modelId != state.model) listener.onSetModel(modelId)
                                },
                                enabled = !state.streaming,
                            ) {
                                Text(stringResource(R.string.compass_save))
                            }
                        }
                    }
                }
            }
        }
        if (state.provider == "openai") {
            // Sign in with ChatGPT: the plan instead of a key (docs/design/ai-providers.md).
            item { SectionTitle(stringResource(R.string.compass_claude_chatgpt_plan)) }
            item {
                Group {
                    if (state.chatgptSignedIn) {
                        ListItem(
                            supportingContent = { if (state.chatgptEmail.isNotEmpty()) Text(state.chatgptEmail) },
                            trailingContent = {
                                TextButton(onClick = { listener.onSignOutOfChatGpt() }) { Text(stringResource(R.string.compass_claude_sign_out)) }
                            },
                            colors = rowColors(),
                        ) { Text(stringResource(R.string.compass_claude_chatgpt_signed_in)) }
                    } else {
                        Row(
                            Modifier.padding(16.dp),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            FilledTonalButton(
                                onClick = { listener.onSignInWithChatGpt() },
                                enabled = !state.chatgptSigningIn,
                            ) { Text(stringResource(R.string.compass_claude_chatgpt_continue)) }
                            if (state.chatgptSigningIn) CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                        }
                    }
                }
            }
            item {
                Text(
                    if (state.chatgptNote.isNotEmpty()) state.chatgptNote
                    else stringResource(R.string.compass_claude_chatgpt_about),
                    style = MaterialTheme.typography.bodySmall,
                    color = if (state.chatgptNote.isNotEmpty()) MaterialTheme.colorScheme.error
                    else MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp),
                )
            }
        }
        item { SectionTitle(stringResource(R.string.compass_claude_api_key_section)) }
        item {
            Group {
                if (state.hasKey) {
                    ListItem(
                        supportingContent = { Text(state.keyHint) },
                        trailingContent = {
                            TextButton(onClick = { listener.onForgetKey() }) {
                                Text(stringResource(R.string.compass_claude_remove_key), color = MaterialTheme.colorScheme.error)
                            }
                        },
                        colors = rowColors(),
                    ) { Text(stringResource(R.string.compass_claude_api_key)) }
                } else {
                    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        // Hidden as typed, with no suggestions or learning.
                        OutlinedSecureTextField(
                            state = keyField,
                            label = { Text(stringResource(if (compatible) R.string.compass_claude_api_key_optional else R.string.compass_claude_api_key)) },
                            placeholder = { Text(state.keyPlaceholder) },
                            modifier = Modifier.fillMaxWidth(),
                        )
                        FilledTonalButton(
                            onClick = {
                                listener.onSetKey(apiKey)
                                keyField.clearText()
                            },
                            enabled = apiKey.isNotEmpty(),
                        ) {
                            Text(stringResource(R.string.compass_claude_save_key))
                        }
                    }
                }
                // A subscription token: the switch it needs, where it's typed (Anthropic's only).
                if (anthropic && apiKey.startsWith("sk-ant-oat")) TokenRisksRow(state, listener)
            }
        }
        item {
            Text(
                stringResource(if (anthropic) R.string.compass_claude_key_note else R.string.compass_claude_key_note_other),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp),
            )
        }
        item { SectionTitle(stringResource(R.string.compass_claude_connection)) }
        item {
            Group {
                // A compatible server's model is typed above; once Test Connection lists its models, they
                // can be picked too.
                if (!compatible || (state.apiModels.isNotEmpty() && state.model.isNotEmpty())) {
                    ModelMenuButton(state.model, modelChoices(state, state.model), !state.streaming) { listener.onSetModel(it) }
                }
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    if (!compatible) {
                        OutlinedTextField(
                            state = urlField,
                            label = { Text(stringResource(R.string.compass_claude_server)) },
                            placeholder = { Text(state.defaultBaseUrl) },
                            supportingText = {
                                Text(
                                    if (anthropic) stringResource(R.string.compass_claude_server_about_anthropic)
                                    else stringResource(R.string.compass_claude_server_about_other, state.providerName)
                                )
                            },
                            lineLimits = TextFieldLineLimits.SingleLine,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                            modifier = Modifier.fillMaxWidth(),
                        )
                    }
                    if (anthropic) {
                        Text(stringResource(R.string.compass_claude_send_key_as), style = MaterialTheme.typography.labelLarge)
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
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                        // The saved URL is normalized (a trailing slash).
                        if (!compatible && baseUrl.trimEnd('/') != state.baseUrl.trimEnd('/')) {
                            FilledTonalButton(onClick = { listener.onSetEndpoint(baseUrl, state.authStyle) }) { Text(stringResource(R.string.compass_save)) }
                        }
                        OutlinedButton(
                            onClick = { listener.onTestConnection() },
                            // A compatible server may need no key.
                            enabled = !state.testing && (state.hasKey || (state.provider == "openai" && state.chatgptSignedIn) ||
                                (compatible && state.baseUrl.isNotEmpty())),
                        ) {
                            Text(stringResource(R.string.compass_claude_test_connection))
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
        item { SectionTitle(stringResource(R.string.compass_claude_files)) }
        item {
            Group {
                val choices =
                    listOf(
                        Triple(0, R.string.compass_claude_files_off, R.string.compass_claude_files_off_about),
                        Triple(1, R.string.compass_claude_files_save, R.string.compass_claude_files_save_about),
                        Triple(2, R.string.compass_claude_files_folder, R.string.compass_claude_files_folder_about),
                    )
                for ((access, title, about) in choices) {
                    ListItem(
                        supportingContent = { Text(stringResource(about)) },
                        leadingContent = {
                            RadioButton(
                                selected = state.filesAccess == access,
                                onClick = { listener.onFiles(access, if (access == 2) folder else "") },
                            )
                        },
                        colors = rowColors(),
                        modifier = Modifier.clickable { listener.onFiles(access, if (access == 2) folder else "") },
                    ) { Text(stringResource(title)) }
                }
                if (state.filesAccess == 2) {
                    Row(
                        Modifier.padding(start = 16.dp, end = 16.dp, bottom = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        OutlinedTextField(
                            state = folderField,
                            label = { Text(stringResource(R.string.compass_claude_folder_in_downloads)) },
                            lineLimits = TextFieldLineLimits.SingleLine,
                            modifier = Modifier.weight(1f),
                        )
                        FilledTonalButton(
                            onClick = { listener.onFiles(2, folder) },
                            enabled = folder.isNotEmpty() && folder != state.filesFolder,
                        ) {
                            Text(stringResource(R.string.compass_claude_use))
                        }
                    }
                }
            }
        }
        item {
            Text(
                stringResource(R.string.compass_claude_files_note),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp),
            )
        }
        item { SectionTitle(stringResource(R.string.compass_claude_advanced)) }
        item {
            Group {
                SwitchRow(
                    stringResource(R.string.compass_claude_devtools),
                    stringResource(R.string.compass_claude_devtools_about),
                    state.devtools,
                ) {
                    listener.onDevTools(it)
                }
                if (anthropic) TokenRisksRow(state, listener)
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
        stringResource(R.string.compass_claude_token_risks),
        stringResource(R.string.compass_claude_token_risks_about),
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
    PageTitle(stringResource(R.string.compass_claude_boosts), onBack)
    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item {
            Group {
                SwitchRow(
                    stringResource(R.string.compass_claude_boosts_allow),
                    stringResource(R.string.compass_claude_boosts_allow_about),
                    state.boostsOn,
                ) {
                    listener.onBoostsAllowed(it)
                }
            }
        }
        item { SectionTitle(stringResource(R.string.compass_claude_your_boosts)) }
        item {
            if (state.boosts.isEmpty()) {
                Text(
                    stringResource(R.string.compass_claude_no_boosts),
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
                                        SymIcon(R.drawable.compass_sym_delete, stringResource(R.string.compass_delete))
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
                Button(onClick = { listener.onAnswer(approval.id, 1) }) { Text(stringResource(R.string.compass_claude_allow)) }
                if (!approval.alwaysAsk) {
                    OutlinedButton(onClick = { listener.onAnswer(approval.id, 2) }) { Text(stringResource(R.string.compass_claude_for_this_chat)) }
                }
                TextButton(onClick = { listener.onAnswer(approval.id, 0) }) { Text(stringResource(R.string.compass_claude_deny)) }
            }
            if (approval.waiting > 0) {
                Text(
                    pluralStringResource(R.plurals.compass_claude_more_waiting, approval.waiting, approval.waiting),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/**
 * Messages written while Claude replies, sent one by one after it: a tap puts one back in the
 * field, × removes it.
 */
@Composable
private fun QueuedList(state: ClaudeChatState, listener: ClaudeChatUi.Listener) {
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(
            stringResource(if (state.streaming) R.string.compass_claude_queued_sends_after else R.string.compass_claude_queued_paused),
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        state.queued.forEachIndexed { index, text ->
            Row(
                Modifier.fillMaxWidth()
                    .clip(RoundedCornerShape(16.dp))
                    .background(MaterialTheme.colorScheme.surfaceContainerHigh),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text,
                    style = MaterialTheme.typography.bodyMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier =
                        Modifier.weight(1f)
                            .clickable(onClickLabel = stringResource(R.string.compass_edit)) { listener.onEditQueued(index) }
                            .padding(horizontal = 14.dp, vertical = 8.dp),
                )
                IconButton(onClick = { listener.onRemoveQueued(index) }, modifier = Modifier.size(36.dp)) {
                    SymIcon(R.drawable.compass_sym_close, stringResource(R.string.compass_remove), Modifier.size(18.dp))
                }
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

/**
 * The sheet before the chat can ask: the provider's key, typed here, or (an OpenAI-compatible
 * server) the way to Settings, where its URL and model go. |onSettings| opens Settings, where
 * another provider can be picked.
 */
@Composable
private fun ColumnScope.KeyEntry(state: ClaudeChatState, listener: ClaudeChatUi.Listener, onSettings: () -> Unit) {
    Column(Modifier.weight(1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        if (state.provider == "compatible") {
            Text(
                stringResource(R.string.compass_claude_compatible_intro),
                style = MaterialTheme.typography.bodyMedium,
            )
            if (state.error.isNotEmpty()) Text(state.error, color = MaterialTheme.colorScheme.error)
            Button(onClick = onSettings) { Text(stringResource(R.string.compass_claude_open_settings)) }
        } else {
            KeyFields(state, listener, onSettings)
        }
    }
}

/** KeyEntry's key field, for a provider that needs a key. */
@Composable
private fun KeyFields(state: ClaudeChatState, listener: ClaudeChatUi.Listener, onSettings: () -> Unit) {
    val keyField = key(state.provider) { rememberTextFieldState() }
    val typed = keyField.text.toString().trim()
    val anthropic = state.provider == "anthropic"
    Text(
        if (anthropic) stringResource(R.string.compass_claude_key_intro_anthropic)
        else stringResource(R.string.compass_claude_key_intro_other, state.providerName),
        style = MaterialTheme.typography.bodyMedium,
    )
    // Hidden as typed, with no suggestions or learning.
    OutlinedSecureTextField(
        state = keyField,
        label = { Text(stringResource(R.string.compass_claude_api_key)) },
        placeholder = { Text(state.keyPlaceholder) },
        modifier = Modifier.fillMaxWidth(),
    )
    if (state.error.isNotEmpty()) Text(state.error, color = MaterialTheme.colorScheme.error)
    // A subscription token: the switch it needs, where it's typed.
    if (anthropic && typed.startsWith("sk-ant-oat")) Group { TokenRisksRow(state, listener) }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
        Button(onClick = { listener.onSetKey(typed) }, enabled = typed.isNotEmpty()) { Text(stringResource(R.string.compass_save)) }
        TextButton(onClick = onSettings) { Text(stringResource(R.string.compass_claude_use_another_provider)) }
    }
}

// What a new chat offers to ask (sent as shown, in the user's language).
private val SUGGESTIONS =
    listOf(
        R.string.compass_claude_suggest_summarize,
        R.string.compass_claude_suggest_key_points,
        R.string.compass_claude_suggest_explain,
    )

@Composable
private fun ColumnScope.Chat(state: ClaudeChatState, listener: ClaudeChatUi.Listener) {
    val input = state.input
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
                    Text(stringResource(R.string.compass_claude_ask_about_page), style = MaterialTheme.typography.headlineSmall)
                    Text(
                        stringResource(R.string.compass_claude_empty_about),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        textAlign = TextAlign.Center,
                    )
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally)) {
                        for (id in SUGGESTIONS) {
                            val suggestion = stringResource(id)
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
                    if (state.canRetry) TextButton(onClick = { listener.onRetry() }) { Text(stringResource(R.string.compass_retry)) }
                }
            }
        }
    }
    state.approval?.let { ApprovalCard(it, listener) }
    if (state.queued.isNotEmpty()) QueuedList(state, listener)
    // What goes with a question: the page, and the whole browser.
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        FilterChip(
            selected = state.attachPage,
            onClick = { listener.onAttachPage(!state.attachPage) },
            label = { Text(stringResource(R.string.compass_claude_this_page)) },
            leadingIcon = { SymIcon(R.drawable.compass_sym_description, null, Modifier.size(FilterChipDefaults.IconSize)) },
        )
        FilterChip(
            selected = state.browserScope,
            onClick = { listener.onBrowserScope(!state.browserScope) },
            label = { Text(stringResource(R.string.compass_claude_whole_browser)) },
            leadingIcon = { SymIcon(R.drawable.compass_sym_public, null, Modifier.size(FilterChipDefaults.IconSize)) },
        )
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        PillField(input, stringResource(R.string.compass_claude_ask_about_page), Modifier.weight(1f))
        if (state.streaming && input.text.isNotBlank()) {
            FilledIconButton(
                onClick = {
                    listener.onSend(input.text.toString().trim())
                    input.clearText()
                },
                colors =
                    IconButtonDefaults.filledIconButtonColors(
                        containerColor = MaterialTheme.colorScheme.secondaryContainer,
                        contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
                    ),
                modifier = Modifier.size(48.dp),
            ) {
                SymIcon(R.drawable.compass_sym_arrow_upward, stringResource(R.string.compass_claude_send_after_reply))
            }
        }
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
                SymIcon(R.drawable.compass_sym_stop, stringResource(R.string.compass_claude_stop))
            }
        } else {
            FilledIconButton(
                onClick = {
                    listener.onSend(input.text.toString().trim())
                    input.clearText()
                },
                enabled = input.text.isNotBlank() || state.queued.isNotEmpty(),
                modifier = Modifier.size(48.dp),
            ) {
                SymIcon(R.drawable.compass_sym_arrow_upward, stringResource(R.string.compass_claude_ask))
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
                stringResource(
                    when (message.kind) {
                        "updated" -> R.string.compass_claude_memory_updated
                        "forgot" -> R.string.compass_claude_memory_forgot
                        else -> R.string.compass_claude_memory_saved
                    },
                    message.text,
                ),
                message.undone,
            ) {
                listener.onUndoMemory(message.undoId)
            }
        "boost" ->
            ChangeRow(
                if (message.kind == "deleted") R.drawable.compass_sym_delete else R.drawable.compass_sym_bolt,
                stringResource(
                    when (message.kind) {
                        "updated" -> R.string.compass_claude_boost_updated
                        "deleted" -> R.string.compass_claude_boost_deleted
                        else -> R.string.compass_claude_boost_saved
                    },
                    message.text,
                ),
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
                stringResource(R.string.compass_claude_undone),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSecondaryContainer,
                modifier = Modifier.padding(end = 12.dp),
            )
        } else {
            TextButton(onClick = onUndo) { Text(stringResource(R.string.compass_undo)) }
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
                toolLabel(message.text).let { label ->
                    val subject = toolSubject(message.input)
                    if (subject.isEmpty()) label else stringResource(R.string.compass_claude_tool_with_subject, label, subject)
                },
                style = MaterialTheme.typography.labelMedium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
            when {
                !message.done -> CircularProgressIndicator(Modifier.size(14.dp), strokeWidth = 2.dp)
                message.error -> SymIcon(R.drawable.compass_sym_error, stringResource(R.string.compass_claude_tool_failed), Modifier.size(16.dp), MaterialTheme.colorScheme.error)
                else -> SymIcon(R.drawable.compass_sym_check_circle, stringResource(R.string.compass_claude_tool_done), Modifier.size(16.dp), MaterialTheme.colorScheme.primary)
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
                            contentDescription = stringResource(R.string.compass_claude_screenshot),
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
            Text(stringResource(if (message.done) R.string.compass_claude_thought else R.string.compass_claude_thinking), style = MaterialTheme.typography.labelMedium, color = muted)
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

/** A few words on what a tool acted on: its URL, ref, text or query, from its input; or "". */
private fun toolSubject(input: String): String {
    val json = runCatching { org.json.JSONObject(input) }.getOrNull() ?: return ""
    for (key in listOf("url", "query", "text", "ref", "tab_id", "path", "name", "selector", "key", "space")) {
        val value = json.optString(key)
        if (value.isNotEmpty()) return if (value.length > 40) value.take(40) + "…" else value
    }
    return ""
}

private fun prettyJson(input: String): String = runCatching { org.json.JSONObject(input).toString(2) }.getOrDefault(input)

/** What a tool did, in words (common/claude/tools.json's names); an unknown tool by its name. */
@Composable
private fun toolLabel(name: String): String {
    val id = toolLabelId(name)
    return if (id == 0) name else stringResource(id)
}

@StringRes
private fun toolLabelId(name: String): Int =
    when (name) {
        "read_page", "get_page_text" -> R.string.compass_claude_tool_reading_page
        "find" -> R.string.compass_claude_tool_finding
        "screenshot" -> R.string.compass_claude_tool_looking_page
        "click" -> R.string.compass_claude_tool_clicking
        "hover" -> R.string.compass_claude_tool_pointing
        "type", "form_input" -> R.string.compass_claude_tool_typing
        "key" -> R.string.compass_claude_tool_key
        "scroll" -> R.string.compass_claude_tool_scrolling
        "navigate" -> R.string.compass_claude_tool_navigating
        "wait" -> R.string.compass_claude_tool_waiting
        "read_console" -> R.string.compass_claude_tool_console
        "run_js" -> R.string.compass_claude_tool_run_js
        "list_tabs" -> R.string.compass_claude_tool_list_tabs
        "open_tab" -> R.string.compass_claude_tool_open_tab
        "select_tab" -> R.string.compass_claude_tool_select_tab
        "close_tab" -> R.string.compass_claude_tool_close_tab
        "activate_tab" -> R.string.compass_claude_tool_activate_tab
        "move_tab" -> R.string.compass_claude_tool_move_tab
        "rename_tab" -> R.string.compass_claude_tool_rename
        "create_folder" -> R.string.compass_claude_tool_create_folder
        "mute_tab" -> R.string.compass_claude_tool_mute_tab
        "split_tabs" -> R.string.compass_claude_tool_split_tabs
        "switch_space" -> R.string.compass_claude_tool_switch_space
        "create_space" -> R.string.compass_claude_tool_create_space
        "search_history" -> R.string.compass_claude_tool_search_history
        "list_downloads" -> R.string.compass_claude_tool_list_downloads
        "send_tab_to_device" -> R.string.compass_claude_tool_send_tab
        "network_requests" -> R.string.compass_claude_tool_network_requests
        "network_response" -> R.string.compass_claude_tool_network_response
        "inspect_element" -> R.string.compass_claude_tool_inspect
        "page_storage" -> R.string.compass_claude_tool_page_storage
        "edit_page_storage" -> R.string.compass_claude_tool_edit_page_storage
        "performance" -> R.string.compass_claude_tool_performance
        "emulate" -> R.string.compass_claude_tool_emulate
        "save_file" -> R.string.compass_claude_tool_save_file
        "list_files" -> R.string.compass_claude_tool_list_files
        "read_file" -> R.string.compass_claude_tool_read_file
        "write_file" -> R.string.compass_claude_tool_write_file
        "list_boosts" -> R.string.compass_claude_tool_list_boosts
        "save_boost" -> R.string.compass_claude_tool_save_boost
        "delete_boost" -> R.string.compass_claude_tool_delete_boost
        else -> 0
    }
