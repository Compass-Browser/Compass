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
import androidx.compose.ui.graphics.Shape
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.RepeatMode
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
import androidx.compose.foundation.layout.fillMaxHeight
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
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.rememberTextFieldState
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
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
    class DropTarget(
        @JvmField val parentId: String,
        @JvmField val beforeId: String,
        @JvmField val rowKey: String,
        @JvmField val edge: Edge,
    )

    /** A row of the sidebar's list as laid out: its key (a node's ID, or a section's), place, height. */
    class ListRow(@JvmField val key: Any, @JvmField val offset: Int, @JvmField val size: Int)

    /** Each pinned and Today entry (not a split's panes): its parent's ID and its siblings. */
    @JvmStatic
    fun entryPlaces(state: SidebarState): Map<String, Pair<String, List<SidebarState.Node>>> {
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
     * Where |nodeId|, dragged with its middle at |y| (the list's pixels), would drop, by the row
     * there: above or below an entry, into a folder (its middle; not a live folder), at the top of
     * Pinned or Today (their headers, or New Tab), or into the favorites. Null: nowhere.
     */
    @JvmStatic
    fun dropTarget(state: SidebarState, rows: List<ListRow>, nodeId: String, y: Float): DropTarget? {
        val over = rows.firstOrNull { y >= it.offset && y < it.offset + it.size } ?: return null
        val key = over.key as? String ?: return null
        when (key) {
            "favorites" -> return DropTarget(state.favoritesId, "", key, Edge.INSIDE)
            "pinned-header", "pinned-empty" ->
                return DropTarget(state.pinnedId, state.pinned.firstOrNull()?.id ?: "", key, Edge.BOTTOM)
            "today-header", "new-tab" ->
                return DropTarget(state.todayId, state.today.firstOrNull()?.id ?: "", key, Edge.BOTTOM)
        }
        if (key == nodeId) return null
        val (parentId, siblings) = entryPlaces(state)[key] ?: return null
        val node = siblings.firstOrNull { it.id == key } ?: return null
        val at = (y - over.offset) / over.size.coerceAtLeast(1)
        if (node.isFolder && !node.isLiveFolder && at > 0.25f && at < 0.75f) {
            return DropTarget(node.id, "", key, Edge.INSIDE)
        }
        if (at < 0.5f) return DropTarget(parentId, node.id, key, Edge.TOP)
        // Below an open folder: at the top of it.
        if (node.isFolder && !node.isLiveFolder && !node.collapsed && node.children.isNotEmpty()) {
            return DropTarget(node.id, node.children.first().id, key, Edge.BOTTOM)
        }
        val next = siblings.getOrNull(siblings.indexOf(node) + 1)
        return DropTarget(parentId, next?.id ?: "", key, Edge.BOTTOM)
    }

    /** A row's part in a drag: picking it up, moving, letting go. */
    internal interface RowDrag {
        fun start(nodeId: String)

        fun move(dy: Float)

        fun end()

        fun cancel()

        fun state(): EntryDrag
    }

    enum class Edge {
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

        /**
         * Split With / Add to Split: the tab entry |tabId| and |targetId| (a tab or a split) become a
         * split, or |tabId| joins it; the split shows.
         */
        fun onSplit(targetId: String, tabId: String)

        /** Separate Tabs: a split's panes go in its place. */
        fun onUnsplit(splitId: String)

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

        fun onEditSpace(spaceId: String, name: String, emoji: String)

        /** The theme editor: the space's whole theme, at most every 50 ms while it's edited. */
        fun onSetSpaceTheme(spaceId: String, theme: SpaceTheme)

        fun onRemoveSpace(spaceId: String)

        /** Moves a space to another profile (its directory's name) with its entries. */
        fun onMoveSpaceToProfile(spaceId: String, profileDir: String)

        fun onNewTab()

        fun onNewIncognitoTab()

        /** New Easel: a board with its tab pinned in the space, opened (docs/design/easels.md). */
        fun onNewEasel()

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

        /** Renames a folder, as the user: Claude doesn't name it again. */
        fun onRenameFolder(folderId: String, name: String)

        /** Undo Name: a folder Claude named gets its name from before back. */
        fun onUndoName(folderId: String)

        /** Suggest a Name: Claude names the folder from its tabs. */
        fun onSuggestName(folderId: String)

        /** Tidy Today: Claude groups the shown space's loose Today entries into named folders. */
        fun onTidyToday()

        /** Undo on Tidy Today's toast in the sidebar. */
        fun onToastUndo()

        fun onDismiss()

        /** The share sheet of a space, a pinned folder or an easel ("space", "folder", "easel"). */
        fun onShare(kind: String, id: String)

        /** Drops a lost shared item's banner. */
        fun onDismissRoomNotice(roomId: String)
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
            SpaceThemed(state.activeSpace?.theme) {
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
            SpaceThemed(state.activeSpace?.theme) {
                Box(
                    Modifier.fillMaxSize()
                        .spaceTint(state.activeSpace?.theme) { it.surfaceContainerLow }
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
                .spaceTint(state.activeSpace?.theme) { it.surfaceContainerLow }
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
        if (state.toast.isNotEmpty()) SidebarToast(state, listener)
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
                if (SidebarUi.entryPlaces(state)[nodeId] == null) return
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
        // Shared items this profile lost: they stay, unshared.
        for ((roomId, text) in state.roomNotices) {
            item(key = "room-notice-$roomId") {
                Row(
                    Modifier.fillMaxWidth()
                        .padding(horizontal = 8.dp, vertical = 4.dp)
                        .clip(RoundedCornerShape(8.dp))
                        .background(CompassStyle.fill)
                        .padding(horizontal = 10.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(text, style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(1f))
                    Box(Modifier.size(28.dp).clip(CircleShape).clickable { listener.onDismissRoomNotice(roomId) }, contentAlignment = Alignment.Center) {
                        Sym(R.drawable.compass_sym_close, 12f)
                    }
                }
            }
        }
        item(key = "pinned-header") { Box(Modifier.dropMark(drag, "pinned-header")) { SectionHeader(stringResource(R.string.compass_sidebar_pinned)) } }
        if (state.pinned.isEmpty()) {
            item(key = "pinned-empty") { Box(Modifier.dropMark(drag, "pinned-empty")) { PinnedEmpty() } }
        } else {
            tree(state.pinned, 0, SidebarUi.EntryPlace.PINNED, state, listener, dragging)
        }
        item(key = "today-header") {
            Box(Modifier.dropMark(drag, "today-header")) {
                SectionHeader(
                    stringResource(R.string.compass_sidebar_today),
                    trailing = if (state.today.isEmpty()) null else stringResource(R.string.compass_clear),
                    onTrailing = { listener.onClearToday() },
                    // Tidy Today (docs/design/ai-sidebar.md): Claude's folders, with Undo.
                    extra =
                        when {
                            state.incognito || !state.tidyAvailable -> null
                            state.tidyBusy -> stringResource(R.string.compass_sidebar_tidying)
                            else -> stringResource(R.string.compass_sidebar_tidy)
                        },
                    extraEnabled = !state.tidyBusy && state.looseTodayEntries >= 5,
                    onExtra = { listener.onTidyToday() },
                )
            }
        }
        item(key = "new-tab") { Box(Modifier.dropMark(drag, "new-tab")) { NewTabRow(listener) } }
        tree(state.today, 0, SidebarUi.EntryPlace.TODAY, state, listener, dragging)
    }
}

/** Where the drag's entry would drop now (SidebarUi.dropTarget()), from the list's rows. */
private fun dropTarget(state: SidebarState, list: LazyListState, drag: SidebarUi.EntryDrag): SidebarUi.DropTarget? {
    val nodeId = drag.nodeId ?: return null
    val rows = list.layoutInfo.visibleItemsInfo.map { SidebarUi.ListRow(it.key, it.offset, it.size) }
    return SidebarUi.dropTarget(state, rows, nodeId, drag.startY + drag.dy)
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
                            .claudeOutline(node.tab >= 0 && node.tab == state.claudeTab, RoundedCornerShape(CompassStyle.tileRadius))
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
    // The profile (name, directory) the space is about to move to, asked first.
    var movingTo by remember { mutableStateOf<Pair<String, String>?>(null) }
    var archive by remember { mutableStateOf(false) }
    var archiveAfter by remember { mutableStateOf(false) }
    Row(
        Modifier.fillMaxWidth().padding(start = 12.dp, top = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Column(Modifier.weight(1f)) {
            Text(space.name, style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            // Its profile, when the chain has more than one (as iOS's space header).
            if (state.profileCount > 1 && space.profile.isNotEmpty()) {
                Text(
                    space.profile,
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        Box {
            Box(
                Modifier.size(32.dp).clip(CircleShape).background(CompassStyle.fill).clickable { menu = true },
                contentAlignment = Alignment.Center,
            ) {
                Sym(R.drawable.compass_sym_more_horiz, 15f)
            }
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                MenuItem(stringResource(R.string.compass_easel_new), R.drawable.compass_sym_easel) {
                    menu = false
                    listener.onNewEasel()
                }
                HorizontalDivider()
                MenuItem(stringResource(R.string.compass_rename), R.drawable.compass_sym_edit) {
                    menu = false
                    renaming = true
                }
                MenuItem(stringResource(R.string.compass_theme_edit), R.drawable.compass_sym_palette) {
                    menu = false
                    theming = true
                }
                MenuItem(stringResource(R.string.compass_sidebar_archive_title), R.drawable.compass_sym_archive) {
                    menu = false
                    archive = true
                }
                if (state.canShare && space.own) {
                    MenuItem(stringResource(if (state.shared.containsKey(space.id)) R.string.compass_sidebar_sharing else R.string.compass_sidebar_share_space), R.drawable.compass_sym_person_add) {
                        menu = false
                        listener.onShare("space", space.id)
                    }
                }
                MenuItem(stringResource(R.string.compass_sidebar_archive_after_ellipsis), R.drawable.compass_sym_schedule) {
                    menu = false
                    archiveAfter = true
                }
                // Move to Profile (docs/design/android.md, "Profiles"): the chain's other
                // profiles; not the window's last space.
                val others = state.allSpaces.filter { !it.own && it.profileDir.isNotEmpty() }
                    .map { it.profile to it.profileDir }.distinct()
                if (state.spaces.size > 1 && others.isNotEmpty()) {
                    HorizontalDivider()
                    for ((name, dir) in others) {
                        MenuItem(if (name.isEmpty()) stringResource(R.string.compass_sidebar_move_to_profile) else stringResource(R.string.compass_sidebar_move_to, name), R.drawable.compass_sym_stacks) {
                            menu = false
                            movingTo = name to dir
                        }
                    }
                }
                if (state.spaces.size > 1) {
                    HorizontalDivider()
                    MenuItem(stringResource(R.string.compass_sidebar_delete_space), R.drawable.compass_sym_delete, destructive = true) {
                        menu = false
                        deleting = true
                    }
                }
            }
        }
    }
    if (renaming) {
        RenameDialog(space.name, onDone = {
            listener.onEditSpace(space.id, it, space.emoji)
            renaming = false
        }) { renaming = false }
    }
    ThemeEditor(
        theming,
        space,
        onTheme = { listener.onSetSpaceTheme(space.id, it) },
        onEmoji = { listener.onEditSpace(space.id, space.name, it) },
        onDismiss = { theming = false },
    )
    ArchiveSheet(archive, state, listener) { archive = false }
    if (archiveAfter) {
        ArchiveAfterDialog(state.archiveAfterHours, onPick = {
            listener.onSetArchiveAfter(it)
            archiveAfter = false
        }) { archiveAfter = false }
    }
    movingTo?.let { (name, dir) ->
        AlertDialog(
            onDismissRequest = { movingTo = null },
            title = {
                Text(
                    if (name.isEmpty()) stringResource(R.string.compass_sidebar_move_space_to_this_profile, space.name)
                    else stringResource(R.string.compass_sidebar_move_space_to, space.name, name)
                )
            },
            text = {
                Text(stringResource(R.string.compass_sidebar_move_space_message))
            },
            confirmButton = {
                TextButton(onClick = {
                    movingTo = null
                    listener.onMoveSpaceToProfile(space.id, dir)
                }) { Text(stringResource(R.string.compass_sidebar_move)) }
            },
            dismissButton = { TextButton(onClick = { movingTo = null }) { Text(stringResource(R.string.compass_cancel)) } },
        )
    }
    if (deleting) {
        AlertDialog(
            onDismissRequest = { deleting = false },
            title = { Text(stringResource(R.string.compass_sidebar_delete_space_title, space.name)) },
            text = { Text(stringResource(R.string.compass_sidebar_delete_space_message)) },
            confirmButton = {
                TextButton(onClick = {
                    deleting = false
                    listener.onRemoveSpace(space.id)
                }) { Text(stringResource(R.string.compass_delete), color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = { TextButton(onClick = { deleting = false }) { Text(stringResource(R.string.compass_cancel)) } },
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
            Text(stringResource(R.string.compass_sidebar_archive_title), style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
            if (state.archive.isNotEmpty()) {
                TextButton(onClick = { listener.onClearArchive() }) { Text(stringResource(R.string.compass_clear)) }
            }
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.compass_done)) }
        }
        if (state.archive.isEmpty()) {
            Text(
                stringResource(R.string.compass_sidebar_nothing_archived),
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
    val choices = listOf(
        12 to pluralStringResource(R.plurals.compass_sidebar_hours, 12, 12),
        24 to pluralStringResource(R.plurals.compass_sidebar_hours, 24, 24),
        7 * 24 to pluralStringResource(R.plurals.compass_sidebar_days, 7, 7),
        30 * 24 to pluralStringResource(R.plurals.compass_sidebar_days, 30, 30),
        0 to stringResource(R.string.compass_never),
    )
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.compass_sidebar_archive_after)) },
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
        confirmButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.compass_cancel)) } },
    )
}

@Composable
private fun RenameDialog(
    name: String,
    title: String = stringResource(R.string.compass_sidebar_rename_space),
    onDone: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    val field = rememberTextFieldState(name)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = { OutlinedTextField(state = field, lineLimits = TextFieldLineLimits.SingleLine) },
        confirmButton = {
            TextButton(onClick = { onDone(field.text.toString().trim().ifEmpty { name }) }) { Text(stringResource(R.string.compass_done)) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.compass_cancel)) } },
    )
}

/** Tidy Today's result over the bar of spaces, as the window's toast (UndoToastUi.kt). */
@Composable
private fun SidebarToast(state: SidebarState, listener: SidebarUi.Listener) {
    Row(
        Modifier.fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 6.dp)
            .clip(RoundedCornerShape(4.dp))
            .background(MaterialTheme.colorScheme.inverseSurface)
            .padding(start = 16.dp, end = 8.dp)
            .heightIn(min = 48.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            state.toast,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.inverseOnSurface,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f).padding(vertical = 8.dp),
        )
        if (state.toastUndoable) {
            TextButton(onClick = { listener.onToastUndo() }) {
                Text(stringResource(R.string.compass_undo), color = MaterialTheme.colorScheme.inversePrimary)
            }
        }
    }
}

/** "PINNED", "TODAY": small, uppercase, dimmed; Today's has Clear, and Tidy before it. */
@Composable
private fun SectionHeader(
    title: String,
    trailing: String? = null,
    onTrailing: () -> Unit = {},
    extra: String? = null,
    extraEnabled: Boolean = true,
    onExtra: () -> Unit = {},
) {
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
        if (extra != null) {
            Text(
                extra,
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary.copy(alpha = if (extraEnabled) 1f else 0.4f),
                modifier =
                    Modifier.clip(CircleShape)
                        .clickable(enabled = extraEnabled, onClick = onExtra)
                        .padding(horizontal = 6.dp, vertical = 2.dp),
            )
        }
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
            stringResource(R.string.compass_sidebar_pinned_empty),
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
        Text(stringResource(R.string.compass_new_tab), style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
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
        // A split's panes share its row (NodeRow).
        if (node.isFolder && !node.collapsed) tree(node.children, depth + 1, place, state, listener, drag)
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
    val split = node.type == "split"
    val untitled = stringResource(R.string.compass_untitled)
    val shown =
        if (split) node.children.any { it.tab >= 0 && it.tab == state.currentTab }
        else node.tab >= 0 && node.tab == state.currentTab
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
                .claudeOutline(
                    if (split) node.children.any { it.tab >= 0 && it.tab == state.claudeTab }
                    else node.tab >= 0 && node.tab == state.claudeTab,
                    shape,
                )
                .combinedClickable(
                    interactionSource = interaction,
                    indication = null,
                    onLongClick = { if (node.type == "tab" || node.isFolder || split) menu = true },
                    onClick = {
                        if (node.isFolder) listener.onToggleFolder(node.id, !node.collapsed)
                        else if (node.type == "tab") listener.onNodeClicked(node.id, node.url)
                        else if (split) node.children.firstOrNull()?.let { listener.onNodeClicked(it.id, it.url) }
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
                // Claude just named it: sparkles for a moment, and the name fades in.
                val sparkling = node.id == state.sparkleId
                val nameAlpha = remember { Animatable(1f) }
                LaunchedEffect(sparkling) {
                    if (sparkling) {
                        nameAlpha.snapTo(0f)
                        nameAlpha.animateTo(1f, tween(600))
                    }
                }
                Box {
                    Sym(
                        when {
                            sparkling -> R.drawable.compass_sym_auto_awesome
                            // A live folder: a plugin fills it, on the devices that run
                            // plugins (not Android yet: docs/design/live-folders.md).
                            node.isLiveFolder -> R.drawable.compass_sym_sync
                            node.collapsed -> R.drawable.compass_sym_folder
                            else -> R.drawable.compass_sym_folder_fill
                        },
                        15f,
                        if (sparkling) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
                    )
                    FolderMenu(menu, { menu = false }, node, state, listener)
                }
                Text(
                    node.title,
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.alpha(nameAlpha.value),
                )
                SharedBadge(state.sharedTooltip(node))
            } else if (split) {
                Box { EntryMenu(menu, { menu = false }, node, place, state, listener) }
                // Its panes side by side, the one on screen marked; a tap opens that pane.
                node.children.forEachIndexed { i, pane ->
                    if (i > 0) {
                        Box(
                            Modifier.size(width = 1.dp, height = 20.dp)
                                .background(MaterialTheme.colorScheme.onSurface.copy(alpha = 0.15f))
                        )
                    }
                    val front = pane.tab >= 0 && pane.tab == state.currentTab
                    Row(
                        Modifier.weight(1f)
                            .fillMaxHeight()
                            .combinedClickable(
                                onLongClick = { menu = true },
                                onClick = { listener.onNodeClicked(pane.id, pane.url) },
                            ),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        SiteIcon(pane.url, 16.dp)
                        Text(
                            pane.shortTitle.ifEmpty { pane.title }.ifEmpty { untitled },
                            style = MaterialTheme.typography.labelLarge,
                            fontWeight = if (front) FontWeight.SemiBold else null,
                            color = if (shown) MaterialTheme.colorScheme.onSecondaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
            } else {
                Box(Modifier.alpha(if (dim) 0.55f else 1f)) {
                    SiteIcon(node.url, CompassStyle.favicon)
                    // Anchored here: in the row itself it would take a slot of its spacing.
                    EntryMenu(menu, { menu = false }, node, place, state, listener)
                }
                Text(
                    // Claude's short title (Shorter Tab Titles), only here: the page keeps its own.
                    node.shortTitle.ifEmpty { node.title }.ifEmpty { untitled },
                    style = MaterialTheme.typography.labelLarge,
                    color = if (shown) MaterialTheme.colorScheme.onSecondaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.alpha(if (dim) 0.55f else 1f),
                )
                if (node.unseen) {
                    Box(Modifier.size(6.dp).clip(CircleShape).background(MaterialTheme.colorScheme.primary))
                }
                SharedBadge(state.sharedTooltip(node))
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

/**
 * A folder's long-press menu: Rename, and Claude's names (docs/design/ai-sidebar.md): Undo Name
 * while its name is Claude's, else Suggest a Name (a folder with entries, not a live one).
 */
@Composable
private fun FolderMenu(
    expanded: Boolean,
    onDismiss: () -> Unit,
    node: SidebarState.Node,
    state: SidebarState,
    listener: SidebarUi.Listener,
) {
    var renaming by remember { mutableStateOf(false) }
    DropdownMenu(expanded = expanded, onDismissRequest = onDismiss) {
        @Composable
        fun Item(label: String, icon: Int, action: () -> Unit) {
            MenuItem(label, icon) {
                onDismiss()
                action()
            }
        }
        Item(stringResource(R.string.compass_rename), R.drawable.compass_sym_edit) { renaming = true }
        // Sharing a pinned folder with other people.
        if (state.canShare && !node.isLiveFolder && state.pinned.any { it.contains(node.id) }) {
            Item(stringResource(if (state.shared.containsKey(node.id)) R.string.compass_sidebar_sharing else R.string.compass_share_ellipsis), R.drawable.compass_sym_person_add) {
                listener.onShare("folder", node.id)
            }
        }
        if (node.canUndoName) {
            Item(stringResource(R.string.compass_sidebar_undo_name), R.drawable.compass_sym_undo) { listener.onUndoName(node.id) }
        } else if (state.suggestNames && !state.incognito && !node.isLiveFolder && node.children.isNotEmpty()) {
            Item(stringResource(R.string.compass_sidebar_suggest_name), R.drawable.compass_sym_auto_awesome) { listener.onSuggestName(node.id) }
        }
    }
    if (renaming) {
        RenameDialog(node.title, title = stringResource(R.string.compass_sidebar_rename_folder), onDone = {
            listener.onRenameFolder(node.id, it)
            renaming = false
        }) { renaming = false }
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
    // Split With and Add to Split list entries beside it (sub: which list shows).
    var sub by remember { mutableStateOf("") }
    val untitled = stringResource(R.string.compass_untitled)
    val splits = remember(expanded, state.pinned, state.today) { splitChoices(node, place, state) }
    DropdownMenu(expanded = expanded, onDismissRequest = {
        sub = ""
        onDismiss()
    }) {
        @Composable
        fun Item(label: String, icon: Int, destructive: Boolean = false, action: () -> Unit) {
            MenuItem(label, icon, destructive) {
                sub = ""
                onDismiss()
                action()
            }
        }
        if (sub.isNotEmpty()) {
            MenuItem(stringResource(if (sub == "with") R.string.compass_sidebar_split_with else R.string.compass_sidebar_add_to_split), R.drawable.compass_sym_chevron_left) { sub = "" }
            HorizontalDivider()
            when {
                sub == "with" -> for (tab in splits.first) {
                    Item(tab.title.ifEmpty { untitled }, R.drawable.compass_sym_split) { listener.onSplit(node.id, tab.id) }
                }
                node.type == "split" -> for (tab in splits.first) {
                    Item(tab.title.ifEmpty { untitled }, R.drawable.compass_sym_split) { listener.onSplit(node.id, tab.id) }
                }
                else -> for (split in splits.second) {
                    Item(split.children.joinToString(" · ") { it.title.ifEmpty { untitled } }, R.drawable.compass_sym_split) {
                        listener.onSplit(split.id, node.id)
                    }
                }
            }
            return@DropdownMenu
        }
        // Splits (docs/design/android.md, "Splits").
        if (node.type == "tab" && splits.first.isNotEmpty()) {
            MenuItem(stringResource(R.string.compass_sidebar_split_with_ellipsis), R.drawable.compass_sym_split) { sub = "with" }
        }
        if ((node.type == "tab" && splits.second.isNotEmpty()) ||
            (node.type == "split" && node.children.size < MAX_SPLIT_PANES && splits.first.isNotEmpty())
        ) {
            MenuItem(stringResource(R.string.compass_sidebar_add_to_split_ellipsis), R.drawable.compass_sym_split_add) { sub = "add" }
        }
        if (node.type == "split") {
            Item(stringResource(R.string.compass_sidebar_separate_tabs), R.drawable.compass_sym_split_off) { listener.onUnsplit(node.id) }
        }
        // An easel's tab: share the board.
        val easel = easelOf(node.url)
        if (easel != null && state.canShare) {
            Item(stringResource(R.string.compass_easel_share), R.drawable.compass_sym_person_add) { listener.onShare("easel", easel) }
        }
        // A pinned tab's or favorite's pinned URL, while it's on another page (sidebar.md, "Pinned URL").
        if (place != SidebarUi.EntryPlace.TODAY && node.isAwayFromSavedUrl) {
            val favorite = place == SidebarUi.EntryPlace.FAVORITES
            Item(
                stringResource(
                    if (favorite) R.string.compass_sidebar_use_page_as_favorite_url
                    else R.string.compass_sidebar_use_page_as_pinned_url
                ),
                R.drawable.compass_sym_keep,
            ) { listener.onUseAsSavedUrl(node.id) }
            Item(
                stringResource(
                    if (favorite) R.string.compass_sidebar_back_to_favorite_url
                    else R.string.compass_sidebar_back_to_pinned_url
                ),
                R.drawable.compass_sym_undo,
            ) { listener.onBackToSavedUrl(node.id) }
        }
        when (place) {
            SidebarUi.EntryPlace.TODAY -> {
                Item(stringResource(R.string.compass_sidebar_pin), R.drawable.compass_sym_keep) { listener.onMoveNode(node.id, "", "pinned") }
                Item(stringResource(R.string.compass_archive), R.drawable.compass_sym_archive) { listener.onArchiveNode(node.id) }
            }
            SidebarUi.EntryPlace.PINNED -> Item(stringResource(R.string.compass_sidebar_unpin), R.drawable.compass_sym_keep_off) { listener.onUnpinNode(node.id) }
            SidebarUi.EntryPlace.FAVORITES -> {}
        }
        if (place != SidebarUi.EntryPlace.FAVORITES) {
            Item(stringResource(R.string.compass_sidebar_add_to_favorites), R.drawable.compass_sym_star) { listener.onMoveNode(node.id, "", "favorites") }
        }
        if (place == SidebarUi.EntryPlace.FAVORITES) {
            Item(stringResource(R.string.compass_sidebar_pin_to_this_space), R.drawable.compass_sym_keep) { listener.onMoveNode(node.id, "", "pinned") }
        }
        for (space in state.spaces) {
            if (space.id == state.activeId) continue
            Item(stringResource(R.string.compass_sidebar_move_to, space.name.ifEmpty { stringResource(R.string.compass_spaces_space) }), R.drawable.compass_sym_stacks) {
                listener.onMoveNode(node.id, space.id, if (place == SidebarUi.EntryPlace.TODAY) "today" else "pinned")
            }
        }
        if (node.url.isNotEmpty()) Item(stringResource(R.string.compass_copy_link), R.drawable.compass_sym_link) { listener.onCopyLink(node.url) }
        HorizontalDivider()
        when (place) {
            SidebarUi.EntryPlace.TODAY ->
                Item(stringResource(R.string.compass_close), R.drawable.compass_sym_delete, destructive = true) { listener.onCloseNode(node.id) }
            SidebarUi.EntryPlace.FAVORITES ->
                Item(stringResource(R.string.compass_sidebar_remove_from_favorites), R.drawable.compass_sym_delete, destructive = true) { listener.onUnpinNode(node.id) }
            SidebarUi.EntryPlace.PINNED ->
                Item(stringResource(R.string.compass_delete), R.drawable.compass_sym_delete, destructive = true) { listener.onUnpinNode(node.id) }
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
            // Every profile's spaces, a wider gap where the profile changes (as iOS's page dots).
            itemsIndexed(state.allSpaces, key = { _, it -> it.id }) { i, space ->
                val selected = space.id == state.activeId
                val newProfile = i > 0 && state.allSpaces[i - 1].profile != space.profile
                val description =
                    if (state.profileCount > 1) stringResource(R.string.compass_spaces_space_in_profile, space.name, space.profile)
                    else space.name
                val scale = remember { Animatable(1f) }
                LaunchedEffect(selected) { scale.animateTo(if (selected) 1.15f else 1f, spring(dampingRatio = 0.6f)) }
                Box(
                    Modifier.padding(start = if (newProfile) 10.dp else 0.dp)
                        .size(34.dp).scale(scale.value).clip(CircleShape).clickable { listener.onSpaceClicked(space.id) }
                        .semantics { contentDescription = description },
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
                val newName = stringResource(R.string.compass_spaces_default_name, state.spaces.size + 1)
                Box(
                    Modifier.size(34.dp).clip(CircleShape).clickable { listener.onAddSpace(newName) },
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

/** A split holds at most four panes (SpaceModel's kMaxSplitPanes). */
private const val MAX_SPLIT_PANES = 4

/**
 * What |node| can split with, as on iOS: the tabs beside it (Split With; for a split, Add to
 * Split), and the splits beside it with room (Add to Split). Not in the favorites or a live folder.
 */
private fun splitChoices(
    node: SidebarState.Node,
    place: SidebarUi.EntryPlace,
    state: SidebarState,
): Pair<List<SidebarState.Node>, List<SidebarState.Node>> {
    val none = Pair(emptyList<SidebarState.Node>(), emptyList<SidebarState.Node>())
    if (place == SidebarUi.EntryPlace.FAVORITES || state.incognito) return none
    val (parentId, siblings) = SidebarUi.entryPlaces(state)[node.id] ?: return none
    if (state.findNode(parentId)?.isLiveFolder == true) return none
    val others = siblings.filter { it.id != node.id }
    return Pair(
        others.filter { it.type == "tab" },
        others.filter { it.type == "split" && it.children.size < MAX_SPLIT_PANES },
    )
}

/**
 * The tab Claude acts on (docs/design/claude.md, "Claude-is-acting indicator"; iOS's outline): a
 * slowly pulsing outline in the theme's tertiary color while it works there.
 */
@Composable
private fun Modifier.claudeOutline(acting: Boolean, shape: Shape): Modifier {
    if (!acting) return this
    val pulse = rememberInfiniteTransition(label = "claude")
    val alpha by
        pulse.animateFloat(
            0.35f,
            1f,
            infiniteRepeatable(tween(900), RepeatMode.Reverse),
            label = "claudeAlpha",
        )
    return border(2.dp, MaterialTheme.colorScheme.tertiary.copy(alpha = alpha), shape)
}

/** A shared item's people badge, which says who it's shared with. */
@Composable
private fun SharedBadge(tooltip: String?) {
    if (tooltip == null) return
    Sym(R.drawable.compass_sym_account_circle, 12f, MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f))
}

/** Is |id| this node or in it. */
private fun SidebarState.Node.contains(id: String): Boolean =
    this.id == id || children.any { it.contains(id) }
