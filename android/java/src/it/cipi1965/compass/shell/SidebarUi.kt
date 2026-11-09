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
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.border
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.TextButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner

/** The sidebar: spaces, favorites, the pinned tree and Today, as on the desktop. */
object SidebarUi {
    /** Where an entry is, for its long-press menu. */
    internal enum class EntryPlace {
        FAVORITES,
        PINNED,
        TODAY,
    }

    interface Listener {
        /** Shows the node's tab, or opens one for it at |url|. */
        fun onNodeClicked(nodeId: String, url: String)

        /** Closes a Today entry's tab (and the entry). */
        fun onCloseNode(nodeId: String)

        fun onUnpinNode(nodeId: String)

        /**
         * Moves an entry, with its tab: to a space's "pinned" tree or "today" (|spaceId| empty:
         * this window's), or to the "favorites".
         */
        fun onMoveNode(nodeId: String, spaceId: String, section: String)

        fun onCopyLink(url: String)

        fun onToggleFolder(folderId: String, collapsed: Boolean)

        fun onSpaceClicked(spaceId: String)

        fun onAddSpace(name: String)

        fun onEditSpace(spaceId: String, name: String, emoji: String, tint: String)

        fun onRemoveSpace(spaceId: String)

        fun onNewTab()

        fun onOpenSync()

        /** Android's setting for the app that fills passwords in other apps (Compass's, if chosen). */
        fun onOpenAutofillSettings()

        /** Pins the tab on screen to the space, or to the favorites. */
        fun onPinCurrentTab(favorite: Boolean)

        fun onDismiss()
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: SidebarState,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) {
                if (state.open) Sidebar(state, listener)
            }
        }
        return view
    }

    /** The sidebar as a permanent pane (wide windows): no scrim, it doesn't close. */
    @JvmStatic
    fun createPaneView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        state: SidebarState,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) { SidebarPanel(state, listener) }
        }
        return view
    }

    /** A space's tint as a color, or null for grey. */
    @JvmStatic
    fun tintColor(tint: String): Color? =
        if (tint.isEmpty()) null else runCatching { Color(android.graphics.Color.parseColor(tint)) }.getOrNull()
}

@Composable
private fun Sidebar(state: SidebarState, listener: SidebarUi.Listener) {
    BackHandler { listener.onDismiss() }
    Box(Modifier.fillMaxSize()) {
        // The scrim closes it.
        Box(
            Modifier.fillMaxSize()
                .background(Color.Black.copy(alpha = 0.32f))
                .clickable(
                    interactionSource = remember { MutableInteractionSource() },
                    indication = null,
                ) {
                    listener.onDismiss()
                }
        )
        SidebarPanel(state, listener, Modifier.width(320.dp))
    }
}

@Composable
private fun SidebarPanel(
    state: SidebarState,
    listener: SidebarUi.Listener,
    modifier: Modifier = Modifier,
) {
    val tint = SidebarUi.tintColor(state.activeSpace?.tint ?: "")
    var editing by remember { mutableStateOf<SidebarState.Space?>(null) }
    editing?.let { space ->
        SpaceEditor(
            space,
            canDelete = state.spaces.size > 1,
            onDone = { name, emoji, color ->
                listener.onEditSpace(space.id, name, emoji, color)
                editing = null
            },
            onDelete = {
                listener.onRemoveSpace(space.id)
                editing = null
            },
            onDismiss = { editing = null },
        )
    }
        Surface(
            color = MaterialTheme.colorScheme.surfaceContainerLow,
            modifier = modifier.fillMaxHeight(),
        ) {
            Box(
                Modifier.fillMaxSize()
                    .background(tint?.copy(alpha = 0.10f) ?: Color.Transparent)
                    .windowInsetsPadding(WindowInsets.safeDrawing)
            ) {
                LazyColumn(Modifier.fillMaxSize(), contentPadding = androidx.compose.foundation.layout.PaddingValues(vertical = 8.dp)) {
                    item { SpaceHeader(state) { editing = state.activeSpace } }
                    item {
                        Row(
                            Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            Chip("Pin tab") { listener.onPinCurrentTab(false) }
                            Chip("Add to favorites") { listener.onPinCurrentTab(true) }
                        }
                    }
                    item { SpaceSwitcher(state, listener) }
                    if (state.favorites.isNotEmpty()) item { Favorites(state, listener) }
                    if (state.pinned.isNotEmpty()) {
                        item { SectionTitle("Pinned") }
                        tree(state.pinned, 0, pinned = true, state, listener)
                    }
                    item { SectionTitle("Today") }
                    item {
                        Row(
                            Modifier.fillMaxWidth()
                                .clickable { listener.onNewTab() }
                                .padding(horizontal = 20.dp, vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(14.dp),
                        ) {
                            Text("+", style = MaterialTheme.typography.titleLarge, color = MaterialTheme.colorScheme.primary)
                            Text("New Tab", style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.primary)
                        }
                    }
                    tree(state.today, 0, pinned = false, state, listener)
                    item {
                        Text(
                            "Sync",
                            style = MaterialTheme.typography.bodyLarge,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier =
                                Modifier.fillMaxWidth()
                                    .padding(top = 24.dp)
                                    .clickable { listener.onOpenSync() }
                                    .padding(horizontal = 20.dp, vertical = 12.dp),
                        )
                        Text(
                            "Passwords in other apps",
                            style = MaterialTheme.typography.bodyLarge,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier =
                                Modifier.fillMaxWidth()
                                    .clickable { listener.onOpenAutofillSettings() }
                                    .padding(horizontal = 20.dp, vertical = 12.dp),
                        )
                    }
                }
            }
        }
}

@Composable
private fun SpaceHeader(state: SidebarState, onEdit: () -> Unit) {
    val space = state.activeSpace ?: return
    Row(
        Modifier.fillMaxWidth()
            .clickable(onClick = onEdit)
            .padding(horizontal = 20.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        if (space.emoji.isNotEmpty()) Text(space.emoji, style = MaterialTheme.typography.titleLarge)
        Text(space.name, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
    }
}

// The palette of the desktop's theme editor (space_model.h): grey, then the tints.
private val PALETTE =
    listOf("", "#3b82f6", "#8b5cf6", "#ec4899", "#ef4444", "#f97316", "#eab308", "#22c55e")

@Composable
private fun SpaceEditor(
    space: SidebarState.Space,
    canDelete: Boolean,
    onDone: (String, String, String) -> Unit,
    onDelete: () -> Unit,
    onDismiss: () -> Unit,
) {
    var name by remember { mutableStateOf(space.name) }
    var emoji by remember { mutableStateOf(space.emoji) }
    var tint by remember { mutableStateOf(space.tint) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Edit space") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedTextField(
                    value = name,
                    onValueChange = { name = it },
                    label = { Text("Name") },
                    singleLine = true,
                )
                OutlinedTextField(
                    value = emoji,
                    onValueChange = { emoji = it.take(8) },
                    label = { Text("Emoji") },
                    singleLine = true,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (color in PALETTE) {
                        val swatch = SidebarUi.tintColor(color) ?: MaterialTheme.colorScheme.outline
                        Box(
                            Modifier.size(28.dp)
                                .clip(CircleShape)
                                .background(swatch)
                                .border(
                                    if (color == tint) 3.dp else 0.dp,
                                    MaterialTheme.colorScheme.onSurface,
                                    CircleShape,
                                )
                                .clickable { tint = color }
                        )
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { onDone(name.trim(), emoji.trim(), tint) }) { Text("Done") }
        },
        dismissButton = {
            Row {
                if (canDelete) TextButton(onClick = onDelete) { Text("Delete") }
                TextButton(onClick = onDismiss) { Text("Cancel") }
            }
        },
    )
}

@Composable
private fun SpaceSwitcher(state: SidebarState, listener: SidebarUi.Listener) {
    LazyRow(
        contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        modifier = Modifier.padding(bottom = 8.dp),
    ) {
        items(state.spaces, key = { it.id }) { space ->
            val active = space.id == state.activeId
            val tint = SidebarUi.tintColor(space.tint) ?: MaterialTheme.colorScheme.outline
            Box(
                Modifier.size(40.dp)
                    .clip(CircleShape)
                    .background(if (active) tint.copy(alpha = 0.35f) else tint.copy(alpha = 0.12f))
                    .clickable { listener.onSpaceClicked(space.id) },
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    space.emoji.ifEmpty { space.name.take(1).uppercase() },
                    style = MaterialTheme.typography.titleMedium,
                )
            }
        }
        item {
            Box(
                Modifier.size(40.dp)
                    .clip(CircleShape)
                    .background(MaterialTheme.colorScheme.surfaceContainerHighest)
                    .clickable { listener.onAddSpace("Space ${state.spaces.size + 1}") },
                contentAlignment = Alignment.Center,
            ) {
                Text("+", style = MaterialTheme.typography.titleMedium)
            }
        }
    }
}

@Composable
private fun Favorites(state: SidebarState, listener: SidebarUi.Listener) {
    LazyRow(
        contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        modifier = Modifier.padding(vertical = 8.dp),
    ) {
        items(state.favorites, key = { it.id }) { node ->
            var menu by remember { mutableStateOf(false) }
            Column(
                Modifier.width(64.dp)
                    .clip(RoundedCornerShape(12.dp))
                    .combinedClickable(
                        onLongClick = { menu = true },
                        onClick = { listener.onNodeClicked(node.id, node.url) },
                    ),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                EntryMenu(menu, { menu = false }, node, SidebarUi.EntryPlace.FAVORITES, state, listener)
                Box(
                    Modifier.size(52.dp)
                        .clip(RoundedCornerShape(14.dp))
                        .background(
                            if (node.tab == state.currentTab) MaterialTheme.colorScheme.primaryContainer
                            else MaterialTheme.colorScheme.surfaceContainerHighest
                        ),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(initial(node), style = MaterialTheme.typography.titleMedium)
                }
                Text(
                    node.title,
                    style = MaterialTheme.typography.labelSmall,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(top = 4.dp),
                )
            }
        }
    }
}

@Composable
private fun Chip(label: String, onClick: () -> Unit) {
    Text(
        label,
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.onSecondaryContainer,
        modifier =
            Modifier.clip(RoundedCornerShape(16.dp))
                .background(MaterialTheme.colorScheme.secondaryContainer)
                .clickable(onClick = onClick)
                .padding(horizontal = 12.dp, vertical = 8.dp),
    )
}

@Composable
private fun SectionTitle(title: String) {
    Text(
        title,
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(start = 20.dp, top = 16.dp, bottom = 4.dp),
    )
}

private fun LazyListScope.tree(
    nodes: List<SidebarState.Node>,
    depth: Int,
    pinned: Boolean,
    state: SidebarState,
    listener: SidebarUi.Listener,
) {
    for (node in nodes) {
        item(key = node.id) { NodeRow(node, depth, pinned, state, listener) }
        if (node.isFolder && !node.collapsed) tree(node.children, depth + 1, pinned, state, listener)
        // A split shows its panes.
        if (node.type == "split") tree(node.children, depth + 1, pinned, state, listener)
    }
}

@Composable
private fun NodeRow(
    node: SidebarState.Node,
    depth: Int,
    pinned: Boolean,
    state: SidebarState,
    listener: SidebarUi.Listener,
) {
    val current = node.tab >= 0 && node.tab == state.currentTab
    var menu by remember { mutableStateOf(false) }
    Row(
        Modifier.fillMaxWidth()
            .padding(horizontal = 8.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(if (current) MaterialTheme.colorScheme.secondaryContainer else Color.Transparent)
            .combinedClickable(
                onLongClick = { if (node.type == "tab") menu = true },
                onClick = {
                    if (node.isFolder) listener.onToggleFolder(node.id, !node.collapsed)
                    else if (node.type == "tab") listener.onNodeClicked(node.id, node.url)
                },
            )
            .padding(start = (12 + depth * 16).dp, end = 4.dp, top = 6.dp, bottom = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Box(
            Modifier.size(28.dp)
                .clip(RoundedCornerShape(8.dp))
                .background(MaterialTheme.colorScheme.surfaceContainerHighest),
            contentAlignment = Alignment.Center,
        ) {
            // Anchored here: in the row itself it would take a slot of its spacing.
            EntryMenu(menu, { menu = false }, node, if (pinned) SidebarUi.EntryPlace.PINNED else SidebarUi.EntryPlace.TODAY, state, listener)
            Text(
                if (node.isFolder) (if (node.collapsed) "▸" else "▾") else initial(node),
                style = MaterialTheme.typography.labelLarge,
            )
        }
        Text(
            node.title.ifEmpty { node.url },
            style = MaterialTheme.typography.bodyLarge,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            // An unloaded pinned tab is dimmer.
            color =
                if (pinned && node.type == "tab" && node.tab < 0) MaterialTheme.colorScheme.onSurfaceVariant
                else MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.weight(1f),
        )
        if (node.type == "tab") {
            Box(
                Modifier.size(36.dp).clip(CircleShape).clickable {
                    if (pinned) listener.onUnpinNode(node.id) else listener.onCloseNode(node.id)
                },
                contentAlignment = Alignment.Center,
            ) {
                Text("×", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else {
            Spacer(Modifier.size(36.dp))
        }
    }
}

/** An entry's long-press menu: where else it can go, its link, and closing it. */
@Composable
private fun EntryMenu(
    expanded: Boolean,
    onDismiss: () -> Unit,
    node: SidebarState.Node,
    place: SidebarUi.EntryPlace,
    state: SidebarState,
    listener: SidebarUi.Listener,
) {
    DropdownMenu(expanded = expanded, onDismissRequest = onDismiss) {
        @Composable
        fun Item(label: String, action: () -> Unit) {
            DropdownMenuItem(
                text = { Text(label) },
                onClick = {
                    onDismiss()
                    action()
                },
            )
        }
        if (place != SidebarUi.EntryPlace.PINNED) {
            Item("Pin to this space") { listener.onMoveNode(node.id, "", "pinned") }
        }
        if (place != SidebarUi.EntryPlace.FAVORITES) {
            Item("Add to favorites") { listener.onMoveNode(node.id, "", "favorites") }
        }
        if (place == SidebarUi.EntryPlace.PINNED && node.tab >= 0) {
            // Back to an ordinary tab: it goes when it closes.
            Item("Move to Today") { listener.onMoveNode(node.id, "", "today") }
        }
        for (space in state.spaces) {
            if (space.id == state.activeId) continue
            val name = listOf(space.emoji, space.name).filter { it.isNotEmpty() }.joinToString(" ")
            Item("Move to ${name.ifEmpty { "space" }}") {
                listener.onMoveNode(node.id, space.id, if (place == SidebarUi.EntryPlace.TODAY) "today" else "pinned")
            }
        }
        if (node.url.isNotEmpty()) Item("Copy link") { listener.onCopyLink(node.url) }
        when {
            place == SidebarUi.EntryPlace.TODAY -> Item("Close tab") { listener.onCloseNode(node.id) }
            else -> Item(if (place == SidebarUi.EntryPlace.FAVORITES) "Remove from favorites" else "Unpin") {
                listener.onUnpinNode(node.id)
            }
        }
    }
}

private fun initial(node: SidebarState.Node): String {
    val text = node.title.ifEmpty { node.url.substringAfter("://").removePrefix("www.") }
    return text.take(1).uppercase().ifEmpty { "•" }
}
