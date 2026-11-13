// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.text.format.DateUtils
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
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
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
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.rememberLazyListState
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
import androidx.compose.material3.RadioButton
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
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.draw.scale
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.zIndex
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

    /** Where a drop goes, and the row that shows it (above it, below it, or into it). */
    internal class DropTarget(
        @JvmField val parentId: String,
        @JvmField val beforeId: String,
        @JvmField val rowKey: String,
        @JvmField val edge: Edge,
    )

    /** A row's part in a drag: picking it up, moving, letting go. */
    internal interface RowDrag {
        fun start(nodeId: String)

        fun move(dy: Float)

        fun end()

        fun cancel()

        fun state(): EntryDrag
    }

    internal enum class Edge {
        TOP,
        BOTTOM,
        INSIDE,
    }

    /**
     * An entry being dragged (docs/design/android.md, P5): a long press picks it up (its menu
     * opens, and goes as the finger moves), and it drops where the finger lets go.
     */
    internal class EntryDrag {
        var nodeId by mutableStateOf<String?>(null)
        /** How far the finger moved, in pixels. */
        var dy by mutableStateOf(0f)
        /** The dragged row's center when it was picked up, in the list's pixels. */
        var startY = 0f
        var target by mutableStateOf<DropTarget?>(null)
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

        /** Archives every Today entry of the space, closing their tabs (Today's "Clear"). */
        fun onClearToday()

        /** Archives a Today entry, closing its tab if it has one (undo brings it back). */
        fun onArchiveNode(nodeId: String)

        /** Puts an archived entry back at the top of Today and opens it at |url|. */
        fun onRestoreArchived(entryId: String, url: String)

        fun onRemoveArchived(entryId: String)

        /** Empties the space's archive. */
        fun onClearArchive()

        /** How long the space's Today entries stay unseen before the archive; 0: never. */
        fun onSetArchiveAfter(hours: Int)

        /** Makes a pinned tab's or favorite's current page its pinned URL. */
        fun onUseAsSavedUrl(nodeId: String)

        /** Takes a pinned tab or favorite back to its pinned URL. */
        fun onBackToSavedUrl(nodeId: String)

        /**
         * Drag and drop: moves an entry, with its tab, into |parentId| (a folder or a root:
         * SidebarState's pinnedId, todayId, favoritesId) before |beforeId| (empty: at the end).
         */
        fun onMoveNodeTo(nodeId: String, parentId: String, beforeId: String)

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
    val list = rememberLazyListState()
    val drag = remember { SidebarUi.EntryDrag() }
    val threshold = with(LocalDensity.current) { 12.dp.toPx() }
    val dragging =
        object : SidebarUi.RowDrag {
            override fun start(nodeId: String) {
                val item = list.layoutInfo.visibleItemsInfo.firstOrNull { it.key == nodeId } ?: return
                if (entryPlaces(state)[nodeId] == null) return
                drag.nodeId = nodeId
                drag.dy = 0f
                drag.startY = item.offset + item.size / 2f
                drag.target = null
            }

            override fun move(dy: Float) {
                if (drag.nodeId == null) return
                drag.dy += dy
                drag.target = if (abs(drag.dy) < threshold) null else dropTarget(state, list, drag)
            }

            override fun end() {
                val nodeId = drag.nodeId
                val target = drag.target
                drag.nodeId = null
                drag.target = null
                drag.dy = 0f
                if (nodeId != null && target != null) listener.onMoveNodeTo(nodeId, target.parentId, target.beforeId)
            }

            override fun cancel() {
                drag.nodeId = null
                drag.target = null
                drag.dy = 0f
            }

            override fun state(): SidebarUi.EntryDrag = drag
        }
    LazyColumn(
        Modifier.fillMaxSize(),
        state = list,
        contentPadding = PaddingValues(start = 12.dp, end = 12.dp, top = 4.dp, bottom = 12.dp),
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        if (state.favorites.isNotEmpty()) {
            item(key = "favorites") { Box(Modifier.dropMark(drag, "favorites")) { FavoritesGrid(state, listener) } }
        }
        item(key = "header") { SpaceHeader(state, listener) }
        item(key = "pinned-header") { Box(Modifier.dropMark(drag, "pinned-header")) { SectionHeader("Pinned") } }
        if (state.pinned.isEmpty()) {
            item(key = "pinned-empty") { Box(Modifier.dropMark(drag, "pinned-empty")) { PinnedEmpty() } }
        } else {
            tree(state.pinned, 0, SidebarUi.EntryPlace.PINNED, state, listener, dragging)
        }
        item(key = "today-header") {
            Box(Modifier.dropMark(drag, "today-header")) {
                SectionHeader("Today", trailing = if (state.today.isEmpty()) null else "Clear") { listener.onClearToday() }
            }
        }
        item(key = "new-tab") { Box(Modifier.dropMark(drag, "new-tab")) { NewTabRow(listener) } }
        tree(state.today, 0, SidebarUi.EntryPlace.TODAY, state, listener, dragging)
    }
}

/** Each pinned and Today entry (not a split's panes): its parent's ID and its siblings. */
private fun entryPlaces(state: SidebarState): Map<String, Pair<String, List<SidebarState.Node>>> {
    val places = HashMap<String, Pair<String, List<SidebarState.Node>>>()
    fun walk(parentId: String, nodes: List<SidebarState.Node>) {
        for (node in nodes) {
            places[node.id] = parentId to nodes
            if (node.isFolder) walk(node.id, node.children)
        }
    }
    walk(state.pinnedId, state.pinned)
    walk(state.todayId, state.today)
    return places
}

/**
 * Where the dragged entry would drop, by the row under its center: above or below an entry, into
 * a folder (its middle), at the top of Pinned or Today (their headers), or into the favorites.
 */
private fun dropTarget(state: SidebarState, list: LazyListState, drag: SidebarUi.EntryDrag): SidebarUi.DropTarget? {
    val y = drag.startY + drag.dy
    val over = list.layoutInfo.visibleItemsInfo.firstOrNull { y >= it.offset && y < it.offset + it.size } ?: return null
    val key = over.key as? String ?: return null
    when (key) {
        "favorites" -> return SidebarUi.DropTarget(state.favoritesId, "", key, SidebarUi.Edge.INSIDE)
        "pinned-header", "pinned-empty" ->
            return SidebarUi.DropTarget(state.pinnedId, state.pinned.firstOrNull()?.id ?: "", key, SidebarUi.Edge.BOTTOM)
        "today-header", "new-tab" ->
            return SidebarUi.DropTarget(state.todayId, state.today.firstOrNull()?.id ?: "", key, SidebarUi.Edge.BOTTOM)
    }
    if (key == drag.nodeId) return null
    val (parentId, siblings) = entryPlaces(state)[key] ?: return null
    val node = siblings.firstOrNull { it.id == key } ?: return null
    val at = (y - over.offset) / over.size.coerceAtLeast(1)
    if (node.isFolder && !node.isLiveFolder && at > 0.25f && at < 0.75f) {
        return SidebarUi.DropTarget(node.id, "", key, SidebarUi.Edge.INSIDE)
    }
    if (at < 0.5f) return SidebarUi.DropTarget(parentId, node.id, key, SidebarUi.Edge.TOP)
    // Below an open folder: at the top of it.
    if (node.isFolder && !node.isLiveFolder && !node.collapsed && node.children.isNotEmpty()) {
        return SidebarUi.DropTarget(node.id, node.children.first().id, key, SidebarUi.Edge.BOTTOM)
    }
    val next = siblings.getOrNull(siblings.indexOf(node) + 1)
    return SidebarUi.DropTarget(parentId, next?.id ?: "", key, SidebarUi.Edge.BOTTOM)
}

/** Where a drop would go, on the row that shows it: a line above or below it, or a fill. */
@Composable
private fun Modifier.dropMark(drag: SidebarUi.EntryDrag, key: String): Modifier {
    val color = MaterialTheme.colorScheme.primary
    val target = drag.target
    if (target == null || target.rowKey != key) return this
    return drawWithContent {
        drawContent()
        val stroke = 2.dp.toPx()
        when (target.edge) {
            SidebarUi.Edge.TOP -> drawRect(color, size = Size(size.width, stroke))
            SidebarUi.Edge.BOTTOM -> drawRect(color, topLeft = Offset(0f, size.height - stroke), size = Size(size.width, stroke))
            SidebarUi.Edge.INSIDE ->
                drawRoundRect(color.copy(alpha = 0.18f), cornerRadius = CornerRadius(10.dp.toPx()))
        }
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

/** The space's name and its ⋯ menu (rename, theme, the archive, delete). */
@Composable
private fun SpaceHeader(state: SidebarState, listener: SidebarUi.Listener) {
    val space = state.activeSpace ?: return
    var menu by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf(false) }
    var theming by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf(false) }
    var archive by remember { mutableStateOf(false) }
    var archiveAfter by remember { mutableStateOf(false) }
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
                MenuItem("Archive", R.drawable.compass_sym_archive) {
                    menu = false
                    archive = true
                }
                MenuItem("Archive Tabs After…", R.drawable.compass_sym_schedule) {
                    menu = false
                    archiveAfter = true
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
    ArchiveSheet(archive, state, listener) { archive = false }
    if (archiveAfter) {
        ArchiveAfterDialog(state.archiveAfterHours, onPick = {
            listener.onSetArchiveAfter(it)
            archiveAfter = false
        }) { archiveAfter = false }
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

/**
 * The space's archive, newest first (SidebarScreen.swift, ArchiveView): a tap puts an entry back at
 * the top of Today and opens it; × drops one; Clear drops them all.
 */
@Composable
private fun ArchiveSheet(open: Boolean, state: SidebarState, listener: SidebarUi.Listener, onDismiss: () -> Unit) {
    CompassSheet(open, onDismiss = onDismiss) {
        Row(
            Modifier.fillMaxWidth().padding(start = 24.dp, end = 12.dp, bottom = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Archive", style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
            if (state.archive.isNotEmpty()) {
                TextButton(onClick = { listener.onClearArchive() }) { Text("Clear") }
            }
            TextButton(onClick = onDismiss) { Text("Done") }
        }
        if (state.archive.isEmpty()) {
            Text(
                "Nothing archived",
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 24.dp, vertical = 24.dp),
            )
        } else {
            LazyColumn(Modifier.fillMaxWidth().heightIn(max = 560.dp), contentPadding = PaddingValues(bottom = 16.dp)) {
                items(state.archive, key = { it.id }) { entry ->
                    Row(
                        Modifier.fillMaxWidth()
                            .heightIn(min = 64.dp)
                            .clickable {
                                onDismiss()
                                listener.onRestoreArchived(entry.id, entry.url)
                            }
                            .padding(start = 24.dp, end = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(16.dp),
                    ) {
                        SiteIcon(entry.url, 20.dp)
                        Column(Modifier.weight(1f)) {
                            Text(
                                entry.title.ifEmpty { entry.url },
                                style = MaterialTheme.typography.bodyLarge,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                            if (entry.archivedAt > 0) {
                                Text(
                                    DateUtils.getRelativeTimeSpanString(
                                            entry.archivedAt,
                                            System.currentTimeMillis(),
                                            DateUtils.MINUTE_IN_MILLIS,
                                        )
                                        .toString(),
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    maxLines = 1,
                                )
                            }
                        }
                        Box(
                            Modifier.size(40.dp).clip(CircleShape).clickable { listener.onRemoveArchived(entry.id) },
                            contentAlignment = Alignment.Center,
                        ) {
                            Sym(R.drawable.compass_sym_close, 14f, MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                }
            }
        }
    }
}

/** "Archive Tabs After": the iOS app's choices (DataViews.swift, ArchiveAfter); 0 hours is never. */
@Composable
private fun ArchiveAfterDialog(hours: Int, onPick: (Int) -> Unit, onDismiss: () -> Unit) {
    val choices = listOf(12 to "12 Hours", 24 to "24 Hours", 7 * 24 to "7 Days", 30 * 24 to "30 Days", 0 to "Never")
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Archive Tabs After") },
        text = {
            Column {
                for ((value, label) in choices) {
                    Row(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).clickable { onPick(value) }.padding(vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(selected = value == hours, onClick = { onPick(value) })
                        Text(label, style = MaterialTheme.typography.bodyLarge)
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
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
    drag: SidebarUi.RowDrag,
) {
    for (node in nodes) {
        item(key = node.id) { NodeRow(node, depth, place, state, listener, drag) }
        if (node.isFolder && !node.collapsed) tree(node.children, depth + 1, place, state, listener, drag)
        // A split shows its panes.
        if (node.type == "split") tree(node.children, depth + 1, place, state, listener, drag)
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
    drag: SidebarUi.RowDrag,
) {
    val shown = node.tab >= 0 && node.tab == state.currentTab
    val dragged = drag.state().nodeId == node.id
    val threshold = with(LocalDensity.current) { 12.dp.toPx() }
    val today = place == SidebarUi.EntryPlace.TODAY && node.type == "tab"
    // A pinned tab with no page open is dimmer.
    val dim = place != SidebarUi.EntryPlace.TODAY && node.type == "tab" && node.tab < 0
    var menu by remember { mutableStateOf(false) }
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val shape = RoundedCornerShape(CompassStyle.rowRadius)
    Box(
        Modifier.fillMaxWidth()
            .zIndex(if (dragged) 1f else 0f)
            .graphicsLayer {
                // The entry follows the finger, lifted.
                translationY = if (dragged) drag.state().dy else 0f
                shadowElevation = if (dragged) 6.dp.toPx() else 0f
                this.shape = shape
                this.clip = dragged
            }
            .dropMark(drag.state(), node.id)
    ) {
        Row(
            Modifier.fillMaxWidth()
                .height(CompassStyle.rowHeight)
                .scale(if (pressed) 0.98f else 1f)
                .clip(shape)
                .background(
                    if (shown || dragged) CompassStyle.active() else if (pressed) CompassStyle.fill else Color.Transparent
                )
                .combinedClickable(
                    interactionSource = interaction,
                    indication = null,
                    onLongClick = { if (node.type == "tab") menu = true },
                    onClick = {
                        if (node.isFolder) listener.onToggleFolder(node.id, !node.collapsed)
                        else if (node.type == "tab") listener.onNodeClicked(node.id, node.url)
                    },
                )
                // Inside the click: it sees the moves first, and the long press's menu goes once
                // the entry is dragged.
                .pointerInput(node.id) {
                    var moved = 0f
                    detectDragGesturesAfterLongPress(
                        onDragStart = {
                            moved = 0f
                            drag.start(node.id)
                        },
                        onDragEnd = { drag.end() },
                        onDragCancel = { drag.cancel() },
                    ) { change, amount ->
                        change.consume()
                        moved += amount.y
                        if (abs(moved) > threshold) menu = false
                        drag.move(amount.y)
                    }
                }
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
        // A pinned tab's or favorite's pinned URL, while it's on another page (sidebar.md, "Pinned URL").
        if (place != SidebarUi.EntryPlace.TODAY && node.isAwayFromSavedUrl) {
            val kind = if (place == SidebarUi.EntryPlace.FAVORITES) "Favorite" else "Pinned"
            Item("Use This Page as $kind URL", R.drawable.compass_sym_keep) { listener.onUseAsSavedUrl(node.id) }
            Item("Back to $kind URL", R.drawable.compass_sym_undo) { listener.onBackToSavedUrl(node.id) }
        }
        when (place) {
            SidebarUi.EntryPlace.TODAY -> {
                Item("Pin", R.drawable.compass_sym_keep) { listener.onMoveNode(node.id, "", "pinned") }
                Item("Archive", R.drawable.compass_sym_archive) { listener.onArchiveNode(node.id) }
            }
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
