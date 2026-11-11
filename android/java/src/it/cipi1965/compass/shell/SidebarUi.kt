// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.statusBars
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
import androidx.compose.material3.BottomSheetDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
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
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.draw.scale
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R
import kotlin.math.abs

/**
 * The sidebar, as the iOS app's (SidebarScreen.swift, SidebarPanel.swift): on phones a panel up
 * from the bottom (full or half height, a drag on its grabber moves it), on wide windows a pane on
 * the left; in the space's tint. The favorites grid, the space's header and menu, Pinned and
 * Today, and a bar with Settings, the spaces and a new tab. A swipe sideways goes to the next
 * space.
 */
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

        fun onNewIncognitoTab()

        /** Closes every Today tab of the space (Today's "Clear"). */
        fun onClearToday()

        fun onOpenSettings()

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
                Sidebar(state, listener)
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
            CompassTheme(dark = isSystemInDarkTheme()) {
                Box(
                    Modifier.fillMaxSize()
                        .spaceTint(state.activeSpace?.tint ?: "", MaterialTheme.colorScheme.surfaceContainerLow)
                        .windowInsetsPadding(WindowInsets.safeDrawing)
                ) {
                    SidebarContent(state, listener, Modifier.padding(top = 8.dp))
                }
            }
        }
        return view
    }

    /** A space's tint as a color, or null for grey. */
    @JvmStatic
    fun tintColor(tint: String): Color? = CompassStyle.parse(tint)
}

/** A Material bottom sheet, in the space's tint, to the top of the screen. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun Sidebar(state: SidebarState, listener: SidebarUi.Listener) {
    CompassSheet(state.open, onDismiss = { listener.onDismiss() }, dragHandle = false, edgeToEdge = true) {
        Column(
            Modifier.fillMaxSize()
                .spaceTint(state.activeSpace?.tint ?: "", MaterialTheme.colorScheme.surfaceContainerLow)
                // Full height: the sheet goes under the status bar, its content doesn't.
                .windowInsetsPadding(WindowInsets.statusBars.union(WindowInsets.navigationBars))
        ) {
            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) { BottomSheetDefaults.DragHandle() }
            SidebarContent(state, listener, Modifier.weight(1f))
        }
    }
}

/** The space's page (a swipe sideways: the next space) over the bar of spaces. */
@Composable
private fun SidebarContent(state: SidebarState, listener: SidebarUi.Listener, modifier: Modifier = Modifier) {
    val index = state.spaces.indexOfFirst { it.id == state.activeId }
    Column(modifier.fillMaxWidth()) {
        AnimatedContent(
            targetState = state.activeId,
            transitionSpec = {
                val forward =
                    state.spaces.indexOfFirst { it.id == targetState } >= state.spaces.indexOfFirst { it.id == initialState }
                (slideInHorizontally(tween(250)) { if (forward) it else -it } + fadeIn(tween(250))) togetherWith
                    (slideOutHorizontally(tween(250)) { if (forward) -it else it } + fadeOut(tween(250)))
            },
            modifier =
                Modifier.weight(1f).pointerInput(index, state.spaces.size) {
                    var dx = 0f
                    detectHorizontalDragGestures(
                        onDragStart = { dx = 0f },
                        onDragEnd = {
                            val next = index + if (dx < 0) 1 else -1
                            if (abs(dx) > 60.dp.toPx() && next in state.spaces.indices) {
                                listener.onSpaceClicked(state.spaces[next].id)
                            }
                        },
                    ) { _, amount -> dx += amount }
                },
            label = "space",
        ) { _ ->
            SpacePage(state, listener)
        }
        SpacesBar(state, listener)
    }
}

@Composable
private fun SpacePage(state: SidebarState, listener: SidebarUi.Listener) {
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 12.dp, end = 12.dp, top = 4.dp, bottom = 12.dp),
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        if (state.favorites.isNotEmpty()) {
            item(key = "favorites") { FavoritesGrid(state, listener) }
        }
        item(key = "header") { SpaceHeader(state, listener) }
        item(key = "pinned-header") { SectionHeader("Pinned") }
        if (state.pinned.isEmpty()) {
            item(key = "pinned-empty") { PinnedEmpty() }
        } else {
            tree(state.pinned, 0, SidebarUi.EntryPlace.PINNED, state, listener)
        }
        item(key = "today-header") {
            SectionHeader("Today", trailing = if (state.today.isEmpty()) null else "Clear") { listener.onClearToday() }
        }
        item(key = "new-tab") { NewTabRow(listener) }
        tree(state.today, 0, SidebarUi.EntryPlace.TODAY, state, listener)
    }
}

/** Favorites, four to a row; a shorter last row's tiles grow to fill it. */
@Composable
private fun FavoritesGrid(state: SidebarState, listener: SidebarUi.Listener) {
    Column(Modifier.padding(top = 8.dp, bottom = 4.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        for (row in state.favorites.chunked(4)) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                for (node in row) {
                    var menu by remember { mutableStateOf(false) }
                    val shown = node.tab >= 0 && node.tab == state.currentTab
                    val interaction = remember { MutableInteractionSource() }
                    val pressed by interaction.collectIsPressedAsState()
                    Box(
                        Modifier.weight(1f)
                            .heightIn(min = CompassStyle.tileHeight)
                            .scale(if (pressed) 0.95f else 1f)
                            .clip(RoundedCornerShape(CompassStyle.tileRadius))
                            .background(if (shown) CompassStyle.active() else CompassStyle.fill)
                            .combinedClickable(
                                interactionSource = interaction,
                                indication = null,
                                onLongClick = { menu = true },
                                onClick = { listener.onNodeClicked(node.id, node.url) },
                            ),
                        contentAlignment = Alignment.Center,
                    ) {
                        SiteIcon(node.url, 26.dp)
                        EntryMenu(menu, { menu = false }, node, SidebarUi.EntryPlace.FAVORITES, state, listener)
                    }
                }
            }
        }
    }
}

/** The space's name and its ⋯ menu (rename, theme, delete). */
@Composable
private fun SpaceHeader(state: SidebarState, listener: SidebarUi.Listener) {
    val space = state.activeSpace ?: return
    var menu by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf(false) }
    var theming by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf(false) }
    Row(
        Modifier.fillMaxWidth().padding(start = 12.dp, top = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(space.name, style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.weight(1f))
        Box {
            Box(
                Modifier.size(32.dp).clip(CircleShape).background(CompassStyle.fill).clickable { menu = true },
                contentAlignment = Alignment.Center,
            ) {
                Sym(R.drawable.compass_sym_more_horiz, 15f)
            }
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                MenuItem("Rename", R.drawable.compass_sym_edit) {
                    menu = false
                    renaming = true
                }
                MenuItem("Edit Theme", R.drawable.compass_sym_palette) {
                    menu = false
                    theming = true
                }
                if (state.spaces.size > 1) {
                    HorizontalDivider()
                    MenuItem("Delete Space", R.drawable.compass_sym_delete, destructive = true) {
                        menu = false
                        deleting = true
                    }
                }
            }
        }
    }
    if (renaming) {
        RenameDialog(space.name, onDone = {
            listener.onEditSpace(space.id, it, space.emoji, space.tint)
            renaming = false
        }) { renaming = false }
    }
    if (theming) {
        ThemeDialog(space, onDone = { emoji, tint ->
            listener.onEditSpace(space.id, space.name, emoji, tint)
            theming = false
        }) { theming = false }
    }
    if (deleting) {
        AlertDialog(
            onDismissRequest = { deleting = false },
            title = { Text("Delete “${space.name}”?") },
            text = { Text("Its pinned tabs and Today go with it, on every device.") },
            confirmButton = {
                TextButton(onClick = {
                    deleting = false
                    listener.onRemoveSpace(space.id)
                }) { Text("Delete", color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = { TextButton(onClick = { deleting = false }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun RenameDialog(name: String, onDone: (String) -> Unit, onDismiss: () -> Unit) {
    var text by remember { mutableStateOf(name) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Rename Space") },
        text = { @Suppress("DEPRECATION") OutlinedTextField(value = text, onValueChange = { text = it }, singleLine = true) },
        confirmButton = { TextButton(onClick = { onDone(text.trim().ifEmpty { name }) }) { Text("Done") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// The palette of the desktop's theme editor (space_model.h): grey, then the tints.
private val PALETTE =
    listOf("", "#3b82f6", "#8b5cf6", "#ec4899", "#ef4444", "#f97316", "#eab308", "#22c55e")

/** The space's icon and color (the desktop's theme editor's palette). */
@Composable
private fun ThemeDialog(space: SidebarState.Space, onDone: (String, String) -> Unit, onDismiss: () -> Unit) {
    var emoji by remember { mutableStateOf(space.emoji) }
    var tint by remember { mutableStateOf(space.tint) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Edit Theme") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
                @Suppress("DEPRECATION") OutlinedTextField(
                    value = emoji,
                    onValueChange = { emoji = it.take(8) },
                    label = { Text("Emoji") },
                    singleLine = true,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (color in PALETTE) {
                        val swatch = CompassStyle.parse(color) ?: CompassStyle.greyAccent
                        Box(
                            Modifier.size(28.dp)
                                .clip(CircleShape)
                                .background(swatch)
                                .border(
                                    if (color == tint) 3.dp else 0.5.dp,
                                    if (color == tint) MaterialTheme.colorScheme.onSurface
                                    else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f),
                                    CircleShape,
                                )
                                .clickable { tint = color }
                        )
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = { onDone(emoji.trim(), tint) }) { Text("Done") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** "PINNED", "TODAY": small, uppercase, dimmed; Today's has Clear. */
@Composable
private fun SectionHeader(title: String, trailing: String? = null, onTrailing: () -> Unit = {}) {
    Row(
        Modifier.fillMaxWidth().padding(start = 12.dp, end = 6.dp, top = 14.dp, bottom = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            title,
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(1f),
        )
        if (trailing != null) {
            Text(
                trailing,
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.clip(CircleShape).clickable(onClick = onTrailing).padding(horizontal = 6.dp, vertical = 2.dp),
            )
        }
    }
}

/** No pinned tabs yet: a dashed box saying how. */
@Composable
private fun PinnedEmpty() {
    val color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.3f)
    Box(
        Modifier.fillMaxWidth()
            .padding(vertical = 2.dp)
            .dashedBorder(color)
            .padding(12.dp)
    ) {
        Text(
            "Pin tabs to keep them in this space.",
            fontSize = 13.sp,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.55f),
        )
    }
}

private fun Modifier.dashedBorder(color: Color): Modifier = drawBehind {
    drawRoundRect(
        color = color,
        cornerRadius = CornerRadius(10.dp.toPx()),
        style = Stroke(width = 1.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(4.dp.toPx(), 3.dp.toPx()))),
    )
}

@Composable
private fun NewTabRow(listener: SidebarUi.Listener) {
    Row(
        Modifier.fillMaxWidth()
            .height(CompassStyle.rowHeight)
            .clip(RoundedCornerShape(CompassStyle.rowRadius))
            .combinedClickable(onLongClick = { listener.onNewIncognitoTab() }, onClick = { listener.onNewTab() })
            .padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Box(Modifier.width(18.dp), contentAlignment = Alignment.Center) { Sym(R.drawable.compass_sym_add, 15f) }
        Text("New Tab", style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

private fun LazyListScope.tree(
    nodes: List<SidebarState.Node>,
    depth: Int,
    place: SidebarUi.EntryPlace,
    state: SidebarState,
    listener: SidebarUi.Listener,
) {
    for (node in nodes) {
        item(key = node.id) { NodeRow(node, depth, place, state, listener) }
        if (node.isFolder && !node.collapsed) tree(node.children, depth + 1, place, state, listener)
        // A split shows its panes.
        if (node.type == "split") tree(node.children, depth + 1, place, state, listener)
    }
}

/** An entry: 44 high, the shown one a light pill with a soft shadow (SidebarScreen.swift, NodeRow). */
@Composable
private fun NodeRow(
    node: SidebarState.Node,
    depth: Int,
    place: SidebarUi.EntryPlace,
    state: SidebarState,
    listener: SidebarUi.Listener,
) {
    val shown = node.tab >= 0 && node.tab == state.currentTab
    val today = place == SidebarUi.EntryPlace.TODAY && node.type == "tab"
    // A pinned tab with no page open is dimmer.
    val dim = place != SidebarUi.EntryPlace.TODAY && node.type == "tab" && node.tab < 0
    var menu by remember { mutableStateOf(false) }
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val shape = RoundedCornerShape(CompassStyle.rowRadius)
    Box(Modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth()
                .height(CompassStyle.rowHeight)
                .scale(if (pressed) 0.98f else 1f)
                .clip(shape)
                .background(if (shown) CompassStyle.active() else if (pressed) CompassStyle.fill else Color.Transparent)
                .combinedClickable(
                    interactionSource = interaction,
                    indication = null,
                    onLongClick = { if (node.type == "tab") menu = true },
                    onClick = {
                        if (node.isFolder) listener.onToggleFolder(node.id, !node.collapsed)
                        else if (node.type == "tab") listener.onNodeClicked(node.id, node.url)
                    },
                )
                .padding(start = 12.dp + CompassStyle.indent * depth, end = if (today) 36.dp else 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            if (node.isFolder) {
                Sym(
                    R.drawable.compass_sym_chevron_right,
                    11f,
                    MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f),
                    Modifier.rotate(if (node.collapsed) 0f else 90f),
                )
                Sym(
                    when {
                        // A live folder: a plugin fills it, on the devices that run
                        // plugins (not Android yet: docs/design/live-folders.md).
                        node.isLiveFolder -> R.drawable.compass_sym_sync
                        node.collapsed -> R.drawable.compass_sym_folder
                        else -> R.drawable.compass_sym_folder_fill
                    },
                    15f,
                    MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
                )
                Text(node.title, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
            } else {
                Box(Modifier.alpha(if (dim) 0.55f else 1f)) {
                    SiteIcon(node.url, CompassStyle.favicon)
                    // Anchored here: in the row itself it would take a slot of its spacing.
                    EntryMenu(menu, { menu = false }, node, place, state, listener)
                }
                Text(
                    node.title.ifEmpty { "Untitled" },
                    style = MaterialTheme.typography.labelLarge,
                    color = if (shown) MaterialTheme.colorScheme.onSecondaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.alpha(if (dim) 0.55f else 1f),
                )
                if (node.unseen) {
                    Box(Modifier.size(6.dp).clip(CircleShape).background(MaterialTheme.colorScheme.primary))
                }
            }
        }
        if (today) {
            Box(
                Modifier.align(Alignment.CenterEnd)
                    .size(width = 40.dp, height = 44.dp)
                    .clip(CircleShape)
                    .clickable { listener.onCloseNode(node.id) },
                contentAlignment = Alignment.Center,
            ) {
                Sym(R.drawable.compass_sym_close, 12f, MaterialTheme.colorScheme.onSurface.copy(alpha = 0.45f))
            }
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
        fun Item(label: String, icon: Int, destructive: Boolean = false, action: () -> Unit) {
            MenuItem(label, icon, destructive) {
                onDismiss()
                action()
            }
        }
        when (place) {
            SidebarUi.EntryPlace.TODAY -> Item("Pin", R.drawable.compass_sym_keep) { listener.onMoveNode(node.id, "", "pinned") }
            SidebarUi.EntryPlace.PINNED -> Item("Unpin", R.drawable.compass_sym_keep_off) { listener.onUnpinNode(node.id) }
            SidebarUi.EntryPlace.FAVORITES -> {}
        }
        if (place != SidebarUi.EntryPlace.FAVORITES) {
            Item("Add to Favorites", R.drawable.compass_sym_star) { listener.onMoveNode(node.id, "", "favorites") }
        }
        if (place == SidebarUi.EntryPlace.FAVORITES) {
            Item("Pin to This Space", R.drawable.compass_sym_keep) { listener.onMoveNode(node.id, "", "pinned") }
        }
        for (space in state.spaces) {
            if (space.id == state.activeId) continue
            Item("Move to ${space.name.ifEmpty { "Space" }}", R.drawable.compass_sym_stacks) {
                listener.onMoveNode(node.id, space.id, if (place == SidebarUi.EntryPlace.TODAY) "today" else "pinned")
            }
        }
        if (node.url.isNotEmpty()) Item("Copy Link", R.drawable.compass_sym_link) { listener.onCopyLink(node.url) }
        HorizontalDivider()
        when (place) {
            SidebarUi.EntryPlace.TODAY ->
                Item("Close", R.drawable.compass_sym_delete, destructive = true) { listener.onCloseNode(node.id) }
            SidebarUi.EntryPlace.FAVORITES ->
                Item("Remove from Favorites", R.drawable.compass_sym_delete, destructive = true) { listener.onUnpinNode(node.id) }
            SidebarUi.EntryPlace.PINNED ->
                Item("Delete", R.drawable.compass_sym_delete, destructive = true) { listener.onUnpinNode(node.id) }
        }
    }
}

/** Settings, the spaces (the shown one larger, in its color), a new space; a new tab. */
@Composable
private fun SpacesBar(state: SidebarState, listener: SidebarUi.Listener) {
    Row(
        Modifier.fillMaxWidth().padding(start = 10.dp, end = 10.dp, bottom = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier.size(44.dp).clip(CircleShape).clickable { listener.onOpenSettings() },
            contentAlignment = Alignment.Center,
        ) {
            Sym(R.drawable.compass_sym_settings, 18f)
        }
        LazyRow(
            Modifier.weight(1f),
            horizontalArrangement = Arrangement.spacedBy(2.dp, Alignment.CenterHorizontally),
            contentPadding = PaddingValues(horizontal = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            items(state.spaces, key = { it.id }) { space ->
                val selected = space.id == state.activeId
                val scale = remember { Animatable(1f) }
                LaunchedEffect(selected) { scale.animateTo(if (selected) 1.15f else 1f, spring(dampingRatio = 0.6f)) }
                Box(
                    Modifier.size(34.dp).scale(scale.value).clip(CircleShape).clickable { listener.onSpaceClicked(space.id) },
                    contentAlignment = Alignment.Center,
                ) {
                    SpaceIcon(
                        space,
                        if (selected) CompassStyle.accent(space.tint)
                        else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.45f),
                    )
                }
            }
            item(key = "add") {
                Box(
                    Modifier.size(34.dp).clip(CircleShape).clickable { listener.onAddSpace("Space ${state.spaces.size + 1}") },
                    contentAlignment = Alignment.Center,
                ) {
                    Sym(R.drawable.compass_sym_add, 15f, MaterialTheme.colorScheme.onSurface.copy(alpha = 0.45f))
                }
            }
        }
        Box(
            Modifier.size(44.dp)
                .clip(CircleShape)
                .combinedClickable(onLongClick = { listener.onNewIncognitoTab() }, onClick = { listener.onNewTab() }),
            contentAlignment = Alignment.Center,
        ) {
            Sym(R.drawable.compass_sym_add, 18f)
        }
    }
}

/** A space's icon: its emoji, or its initial (SpaceIcon). */
@Composable
private fun SpaceIcon(space: SidebarState.Space, color: Color) {
    if (space.emoji.isNotEmpty()) {
        Text(space.emoji, fontSize = 20.sp)
    } else {
        Text(
            space.name.take(1).uppercase().ifEmpty { "•" },
            fontSize = 20.sp,
            fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.SansSerif,
            color = color,
        )
    }
}
