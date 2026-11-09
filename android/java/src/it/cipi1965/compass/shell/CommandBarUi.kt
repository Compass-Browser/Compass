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
import androidx.compose.foundation.border
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
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.TextButton
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
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
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R

/**
 * The command bar, as the iOS app's (CommandBar.swift): a sheet in the space's tint with the field
 * ("New tab in <space>", or "Go to" for the page's address, all selected) and cards: the typed
 * address or search, the sidebar's entries to switch to, search suggestions and history.
 */
object CommandBarUi {
    interface Listener {
        fun onTextChanged(text: String)

        fun onSuggestionClicked(index: Int)

        fun onEntryClicked(id: String, url: String)

        fun onSubmit(text: String)

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
            CompassTheme(dark = state.incognito || isSystemInDarkTheme()) {
                if (state.open) CommandBar(state, listener)
            }
        }
        return view
    }
}

@Composable
private fun CommandBar(state: CommandBarState, listener: CommandBarUi.Listener) {
    BackHandler { listener.onDismiss() }
    Box(Modifier.fillMaxSize()) {
        Box(
            Modifier.fillMaxSize()
                .background(Color.Black.copy(alpha = 0.25f))
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {
                    listener.onDismiss()
                }
        )
        Column(
            Modifier.fillMaxSize()
                .windowInsetsPadding(WindowInsets.statusBars)
                .padding(top = 10.dp)
                .clip(RoundedCornerShape(topStart = 28.dp, topEnd = 28.dp))
                .background(MaterialTheme.colorScheme.surfaceContainerLow)
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {}
                .windowInsetsPadding(WindowInsets.navigationBars.union(WindowInsets.ime))
        ) {
            Header(state, listener)
            Results(state, listener)
        }
    }
}

// The TextFieldValue overload of BasicTextField: the page's address starts all selected.
@Suppress("DEPRECATION")
@Composable
private fun Header(state: CommandBarState, listener: CommandBarUi.Listener) {
    val focus = remember { FocusRequester() }
    val keyboard = LocalSoftwareKeyboardController.current
    // The page's address starts all selected, to type over.
    var value by remember { mutableStateOf(TextFieldValue(state.text, TextRange(0, state.text.length))) }
    LaunchedEffect(Unit) {
        focus.requestFocus()
        keyboard?.show()
    }
    Column(
        Modifier.fillMaxWidth().padding(start = 14.dp, end = 8.dp, top = 12.dp, bottom = 10.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text(
            if (state.editing) "Go to" else if (state.incognito) "New incognito tab" else "New tab in ${state.spaceName.ifEmpty { "this space" }}",
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(start = 16.dp),
        )
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(
                Modifier.weight(1f)
                    .height(56.dp)
                    .clip(CircleShape)
                    .background(MaterialTheme.colorScheme.surfaceContainerHigh)
                    .padding(horizontal = 16.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Sym(R.drawable.compass_sym_search, 16f, MaterialTheme.colorScheme.onSurfaceVariant)
                Box(Modifier.weight(1f)) {
                    if (value.text.isEmpty()) {
                        Text("Search or enter address", style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
                    }
                    BasicTextField(
                        value = value,
                        onValueChange = {
                            value = it
                            if (it.text != state.text) listener.onTextChanged(it.text)
                        },
                        singleLine = true,
                        textStyle = MaterialTheme.typography.bodyLarge.copy(color = MaterialTheme.colorScheme.onSurface),
                        cursorBrush = SolidColor(MaterialTheme.colorScheme.primary),
                        keyboardOptions =
                            KeyboardOptions(
                                capitalization = KeyboardCapitalization.None,
                                autoCorrectEnabled = false,
                                keyboardType = KeyboardType.Uri,
                                imeAction = ImeAction.Go,
                            ),
                        keyboardActions = KeyboardActions(onGo = { listener.onSubmit(value.text) }),
                        modifier = Modifier.fillMaxWidth().focusRequester(focus),
                    )
                }
                if (value.text.isNotEmpty()) {
                    Box(
                        Modifier.size(28.dp).clip(RoundedCornerShape(14.dp)).clickable {
                            value = TextFieldValue("")
                            listener.onTextChanged("")
                        },
                        contentAlignment = Alignment.Center,
                    ) {
                        Sym(R.drawable.compass_sym_close, 14f, MaterialTheme.colorScheme.onSurface.copy(alpha = 0.3f))
                    }
                }
            }
            TextButton(onClick = { listener.onDismiss() }) { Text("Cancel") }
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
        if (text.isNotEmpty()) {
            item(key = "typed") {
                Card(Modifier.padding(bottom = 8.dp)) {
                    CardRow(
                        text.removePrefix("https://").removePrefix("http://").removePrefix("www."),
                        subtitle = if (state.typedIsAddress) "Open address" else "Search the web",
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
                CardSection(if (text.isEmpty()) "Today" else "Switch to tab") {
                    state.entries.forEachIndexed { i, entry ->
                        CardRow(
                            entry.title.ifEmpty { entry.url },
                            subtitle = if (entry.where == "Favorites") "Favorites" else "${state.spaceName} · ${entry.where}",
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
                CardSection("Search suggestions") {
                    searches.forEachIndexed { i, (index, suggestion) ->
                        CardRow(suggestion.title, first = i == 0, onClick = { listener.onSuggestionClicked(index) }) {
                            Box(
                                Modifier.size(32.dp).clip(RoundedCornerShape(9.dp)).background(MaterialTheme.colorScheme.onSurface.copy(alpha = 0.06f)),
                                contentAlignment = Alignment.Center,
                            ) {
                                Sym(R.drawable.compass_sym_search, 15f, MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                }
            }
        }
        if (pages.isNotEmpty()) {
            item(key = "pages") {
                CardSection(if (text.isEmpty()) "Recently visited" else "History") {
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
    }
}
