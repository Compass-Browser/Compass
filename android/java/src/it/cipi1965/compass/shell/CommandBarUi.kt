// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.compose.BackHandler
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.IconButton
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.TextFieldState
import androidx.compose.foundation.text.input.clearText
import androidx.compose.runtime.snapshotFlow
import it.cipi1965.compass.R

/**
 * The command bar, as the iOS app's (CommandBar.swift): a sheet in the space's tint with the field
 * ("New tab in <space>", or "Go to" for the page's address, all selected) and cards: the typed
 * address or search, the sidebar's entries to switch to, search suggestions, history and tabs on
 * other devices.
 */
object CommandBarUi {
    interface Listener {
        fun onTextChanged(text: String)

        fun onSuggestionClicked(index: Int)

        fun onEntryClicked(id: String, url: String)

        /** A tab on another device: opens its page as a new Today entry. */
        fun onDeviceTabClicked(url: String)

        fun onSubmit(text: String)

        /** Ask Claude the typed question (Ask in the command bar). */
        fun onAsk()

        /** "About this page" on Ask's row: the page's title and host go with the question. */
        fun onAboutPage(on: Boolean)

        /** Continue in Chat: the Claude sheet, on a chat that starts with the question and answer. */
        fun onContinueInChat()

        fun onDismiss()
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: CommandBarState,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            SpaceThemed(if (state.incognito) null else state.theme, forceDark = state.incognito) {
                if (state.open) CommandBar(state, listener)
            }
        }
        return view
    }
}

@Composable
private fun CommandBar(state: CommandBarState, listener: CommandBarUi.Listener) {
    BackHandler { listener.onDismiss() }
    // Material's full-screen search: the field at the top, the results under it.
    Column(
        Modifier.fillMaxSize()
            .spaceTint(if (state.incognito) null else state.theme) { it.surface }
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {}
            .windowInsetsPadding(WindowInsets.statusBars.union(WindowInsets.navigationBars).union(WindowInsets.ime))
    ) {
        Header(state, listener)
        Results(state, listener)
    }
}

// The TextFieldValue overload of BasicTextField: the page's address starts all selected.
@Composable
private fun Header(state: CommandBarState, listener: CommandBarUi.Listener) {
    val focus = remember { FocusRequester() }
    val keyboard = LocalSoftwareKeyboardController.current
    // The page's address starts all selected, to type over.
    val field = remember { TextFieldState(state.text, TextRange(0, state.text.length)) }
    val text = field.text.toString()
    LaunchedEffect(Unit) {
        focus.requestFocus()
        keyboard?.show()
        snapshotFlow { field.text.toString() }.collect { if (it != state.text) listener.onTextChanged(it) }
    }
    Column(
        Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 8.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Row(
                Modifier.weight(1f)
                    .height(56.dp)
                    .clip(CircleShape)
                    .background(MaterialTheme.colorScheme.surfaceContainerHigh)
                    .padding(start = 4.dp, end = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                IconButton(onClick = { listener.onDismiss() }) {
                    Sym(R.drawable.compass_sym_arrow_back, 20f, MaterialTheme.colorScheme.onSurface)
                }
                Box(Modifier.weight(1f)) {
                    if (text.isEmpty()) {
                        Text(stringResource(R.string.compass_bar_search_or_enter_address), style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
                    }
                    BasicTextField(
                        state = field,
                        lineLimits = TextFieldLineLimits.SingleLine,
                        textStyle = MaterialTheme.typography.bodyLarge.copy(color = MaterialTheme.colorScheme.onSurface),
                        cursorBrush = SolidColor(MaterialTheme.colorScheme.primary),
                        keyboardOptions =
                            KeyboardOptions(
                                capitalization = KeyboardCapitalization.None,
                                autoCorrectEnabled = false,
                                keyboardType = KeyboardType.Uri,
                                imeAction = ImeAction.Go,
                            ),
                        onKeyboardAction = { listener.onSubmit(field.text.toString()) },
                        modifier = Modifier.fillMaxWidth().focusRequester(focus),
                    )
                }
                if (text.isNotEmpty()) {
                    IconButton(onClick = { field.clearText() }) {
                        Sym(R.drawable.compass_sym_close, 20f, MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
        }
        Text(
            when {
                state.editing -> stringResource(R.string.compass_command_go_to)
                state.incognito -> stringResource(R.string.compass_command_new_incognito_tab)
                else ->
                    stringResource(
                        R.string.compass_command_new_tab_in,
                        state.spaceName.ifEmpty { stringResource(R.string.compass_command_this_space) },
                    )
            },
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.padding(start = 16.dp),
        )
    }
}

/**
 * Claude's answer to the question (Ask in the command bar): plain text as it streams, about six
 * lines then it scrolls; "Claude is answering…" until the first words, or why it failed; and
 * Continue in Chat.
 */
@Composable
private fun AnswerCard(state: CommandBarState, listener: CommandBarUi.Listener) {
    Card(Modifier.padding(top = 4.dp)) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Sym(R.drawable.compass_sym_auto_awesome, 16f, MaterialTheme.colorScheme.primary)
                Text(
                    state.asked,
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            when {
                state.answerError.isNotEmpty() && state.answer.isEmpty() ->
                    Text(state.answerError, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error)
                state.answer.isEmpty() ->
                    Text(stringResource(R.string.compass_command_claude_answering), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                else ->
                    Text(
                        state.answer,
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.heightIn(max = 160.dp).verticalScroll(rememberScrollState()),
                    )
            }
            if (state.answerDone && state.answer.isNotEmpty()) {
                TextButton(onClick = { listener.onContinueInChat() }) { Text(stringResource(R.string.compass_command_continue_in_chat)) }
            }
        }
    }
}

@Composable
private fun Results(state: CommandBarState, listener: CommandBarUi.Listener) {
    val text = state.text.trim()
    val searches = state.suggestions.withIndex().filter { it.value.isSearch && it.value.title != text }
    val pages = state.suggestions.withIndex().filter { !it.value.isSearch }
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 14.dp, end = 14.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        // Claude's answer replaces the results until the text changes.
        if (state.asked.isNotEmpty()) {
            item(key = "answer") { AnswerCard(state, listener) }
            return@LazyColumn
        }
        if (state.askOffered && text.isNotEmpty()) {
            item(key = "ask") {
                Card(Modifier.padding(bottom = 8.dp)) {
                    CardRow(
                        state.askTitle,
                        subtitle = text,
                        first = true,
                        trailing = R.drawable.compass_sym_keyboard_return,
                        onClick = { listener.onAsk() },
                    ) {
                        MenuIcon(R.drawable.compass_sym_auto_awesome)
                    }
                    if (state.aboutPageTitle.isNotEmpty()) {
                        Row(
                            Modifier.fillMaxWidth().padding(start = 72.dp, end = 16.dp, bottom = 8.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(
                                stringResource(R.string.compass_command_about_this_page, state.aboutPageTitle),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                modifier = Modifier.weight(1f),
                            )
                            Switch(checked = state.aboutPage, onCheckedChange = { listener.onAboutPage(it) })
                        }
                    }
                }
            }
        }
        if (text.isNotEmpty()) {
            item(key = "typed") {
                Card(Modifier.padding(bottom = 8.dp)) {
                    CardRow(
                        text.removePrefix("https://").removePrefix("http://").removePrefix("www."),
                        subtitle = stringResource(if (state.typedIsAddress) R.string.compass_command_open_address else R.string.compass_command_search_web),
                        first = true,
                        trailing = R.drawable.compass_sym_keyboard_return,
                        onClick = { listener.onSubmit(text) },
                    ) {
                        MenuIcon(if (state.typedIsAddress) R.drawable.compass_sym_public else R.drawable.compass_sym_search)
                    }
                }
            }
        }
        if (state.entries.isNotEmpty()) {
            item(key = "entries") {
                CardSection(stringResource(if (text.isEmpty()) R.string.compass_command_today else R.string.compass_command_switch_to_tab)) {
                    state.entries.forEachIndexed { i, entry ->
                        CardRow(
                            entry.title.ifEmpty { entry.url },
                            subtitle =
                                if (entry.where == "Favorites") whereLabel(entry.where)
                                else stringResource(R.string.compass_command_entry_where, state.spaceName, whereLabel(entry.where)),
                            first = i == 0,
                            trailing = R.drawable.compass_sym_arrow_forward,
                            onClick = { listener.onEntryClicked(entry.id, entry.url) },
                        ) {
                            SiteTile(entry.url)
                        }
                    }
                }
            }
        }
        if (searches.isNotEmpty()) {
            item(key = "searches") {
                CardSection(stringResource(R.string.compass_command_search_suggestions)) {
                    searches.forEachIndexed { i, (index, suggestion) ->
                        CardRow(suggestion.title, first = i == 0, onClick = { listener.onSuggestionClicked(index) }) {
                            MenuIcon(R.drawable.compass_sym_search)
                        }
                    }
                }
            }
        }
        if (pages.isNotEmpty()) {
            item(key = "pages") {
                CardSection(stringResource(if (text.isEmpty()) R.string.compass_command_recently_visited else R.string.compass_menu_history)) {
                    pages.forEachIndexed { i, (index, suggestion) ->
                        CardRow(
                            suggestion.title,
                            subtitle = suggestion.subtitle,
                            first = i == 0,
                            onClick = { listener.onSuggestionClicked(index) },
                        ) {
                            SiteTile(suggestion.url)
                        }
                    }
                }
            }
        }
        if (state.deviceTabs.isNotEmpty()) {
            item(key = "devices") {
                CardSection(stringResource(R.string.compass_command_tabs_on_other_devices)) {
                    state.deviceTabs.forEachIndexed { i, tab ->
                        CardRow(
                            tab.title.ifEmpty { tab.url },
                            subtitle = stringResource(R.string.compass_command_on_device, tab.device, tab.url.removePrefix("https://").removePrefix("www.")),
                            first = i == 0,
                            onClick = { listener.onDeviceTabClicked(tab.url) },
                        ) {
                            SiteTile(tab.url)
                        }
                    }
                }
            }
        }
    }
}

/** An entry's place for its subtitle: |where| is CompassBar's key ("Favorites", "Pinned", "Today"). */
@Composable
private fun whereLabel(where: String): String =
    when (where) {
        "Favorites" -> stringResource(R.string.compass_command_favorites)
        "Pinned" -> stringResource(R.string.compass_command_pinned)
        "Today" -> stringResource(R.string.compass_command_today)
        else -> where
    }
