// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.compose.BackHandler
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.gestures.snapping.rememberSnapFlingBehavior
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.compositeOver
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.min
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import kotlin.math.roundToInt
import kotlinx.coroutines.launch

/**
 * The tab picker (docs/design/android.md, "Tab picker"), as on iOS: the window's tabs as cards
 * in a row over the space's tint, least recently shown at the left, the tab on screen centered.
 * A tap shows a card's tab, a flick up closes it, a drag down on the background closes the
 * picker.
 */
object TabPickerUi {
    interface Listener {
        fun onSelect(id: Int)

        fun onClose(id: Int)

        fun onNewTab()

        fun onIncognito(incognito: Boolean)

        fun onDismiss()
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: TabPickerState,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme() || state.incognito) {
                if (state.open) TabPicker(state, listener)
            }
        }
        return view
    }
}

@Composable
private fun TabPicker(state: TabPickerState, listener: TabPickerUi.Listener) {
    BackHandler { listener.onDismiss() }
    val surface = MaterialTheme.colorScheme.surface
    val background =
        SidebarUi.tintColor(state.tint)?.takeIf { !state.incognito }?.copy(alpha = 0.22f)?.compositeOver(surface)
            ?: MaterialTheme.colorScheme.surfaceContainer
    val pull = remember { mutableFloatStateOf(0f) }
    val dismissPx = with(LocalDensity.current) { 120.dp.toPx() }
    Surface(
        color = background,
        modifier =
            Modifier.fillMaxSize()
                // A drag down off the cards closes the picker.
                .pointerInput(Unit) {
                    detectVerticalDragGestures(
                        onDragEnd = {
                            if (pull.floatValue > dismissPx) listener.onDismiss()
                            pull.floatValue = 0f
                        },
                        onDragCancel = { pull.floatValue = 0f },
                    ) { change, dy ->
                        change.consume()
                        pull.floatValue = (pull.floatValue + dy).coerceAtLeast(0f)
                    }
                },
    ) {
        Column(
            Modifier.fillMaxSize()
                .windowInsetsPadding(WindowInsets.statusBars)
                .windowInsetsPadding(WindowInsets.navigationBars)
                .offset { IntOffset(0, (pull.floatValue / 3).roundToInt()) }
        ) {
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    if (state.cards.size == 1) "1 tab" else "${state.cards.size} tabs",
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.weight(1f),
                )
                if (state.hasIncognito) {
                    FilterChip(
                        selected = !state.incognito,
                        onClick = { listener.onIncognito(false) },
                        label = { Text("Tabs") },
                    )
                    FilterChip(
                        selected = state.incognito,
                        onClick = { listener.onIncognito(true) },
                        label = { Text("Incognito") },
                    )
                }
            }
            Cards(state, listener, Modifier.weight(1f).fillMaxWidth())
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                TextButton(onClick = { listener.onIncognito(!state.incognito) }) {
                    Text(if (state.incognito) "Leave incognito" else "New incognito tab")
                }
                Spacer(Modifier.weight(1f))
                FilledTonalButton(onClick = { listener.onNewTab() }) { Text("+  New tab") }
                Spacer(Modifier.width(8.dp))
                TextButton(onClick = { listener.onDismiss() }) { Text("Done") }
            }
        }
    }
}

@Composable
private fun Cards(state: TabPickerState, listener: TabPickerUi.Listener, modifier: Modifier) {
    BoxWithConstraints(modifier, contentAlignment = Alignment.Center) {
        if (state.cards.isEmpty()) {
            Text(
                if (state.incognito) "No incognito tabs" else "No tabs",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            return@BoxWithConstraints
        }
        // A card is the page's shape, as large as fits with a title row above it.
        val titleRow = 40.dp
        val cardWidth = min(maxWidth * 0.72f, (maxHeight - titleRow - 24.dp) * state.aspect)
        val cardHeight = cardWidth / state.aspect
        val side = (maxWidth - cardWidth) / 2
        val initial = state.cards.indexOfFirst { it.id == state.currentId }.coerceAtLeast(0)
        val listState = rememberLazyListState(initialFirstVisibleItemIndex = initial)
        LazyRow(
            state = listState,
            flingBehavior = rememberSnapFlingBehavior(listState),
            contentPadding = PaddingValues(horizontal = side),
            horizontalArrangement = Arrangement.spacedBy(16.dp),
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxSize(),
        ) {
            items(state.cards, key = { it.id }) { card ->
                Card(
                    card,
                    state,
                    current = card.id == state.currentId,
                    width = cardWidth,
                    height = cardHeight,
                    listener = listener,
                )
            }
        }
    }
}

@Composable
private fun Card(
    card: TabPickerState.Card,
    state: TabPickerState,
    current: Boolean,
    width: androidx.compose.ui.unit.Dp,
    height: androidx.compose.ui.unit.Dp,
    listener: TabPickerUi.Listener,
) {
    val scope = rememberCoroutineScope()
    // Under the finger: set as it moves, so the release sees where it is.
    val drag = remember(card.id) { mutableFloatStateOf(0f) }
    // After the release: back, or off the top.
    val settle = remember(card.id) { Animatable(0f) }
    val heightPx = with(LocalDensity.current) { height.toPx() }
    val lift = drag.floatValue + settle.value
    Column(
        Modifier.width(width)
            .offset { IntOffset(0, (drag.floatValue + settle.value).roundToInt()) }
            .alpha((1f + lift / heightPx).coerceIn(0f, 1f))
            // A flick up closes the tab.
            .pointerInput(card.id) {
                detectVerticalDragGestures(
                    onDragEnd = {
                        val released = drag.floatValue
                        drag.floatValue = 0f
                        scope.launch {
                            settle.snapTo(released)
                            if (released < -heightPx * 0.2f) {
                                settle.animateTo(-heightPx * 1.2f, tween(160))
                                listener.onClose(card.id)
                            } else {
                                settle.animateTo(0f)
                            }
                        }
                    },
                    onDragCancel = {
                        val released = drag.floatValue
                        drag.floatValue = 0f
                        scope.launch {
                            settle.snapTo(released)
                            settle.animateTo(0f)
                        }
                    },
                ) { change, dy ->
                    change.consume()
                    drag.floatValue = (drag.floatValue + dy).coerceAtMost(0f)
                }
            }
    ) {
        Row(
            Modifier.fillMaxWidth().height(40.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    card.title.ifEmpty { card.host },
                    style = MaterialTheme.typography.labelLarge,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                if (card.host.isNotEmpty() && card.title.isNotEmpty()) {
                    Text(
                        card.host,
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
            Box(
                Modifier.size(32.dp).clip(CircleShape).clickable { listener.onClose(card.id) },
                contentAlignment = Alignment.Center,
            ) {
                Text("✕", color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        val shape = RoundedCornerShape(16.dp)
        Box(
            Modifier.width(width)
                .height(height)
                .clip(shape)
                .background(MaterialTheme.colorScheme.surfaceContainerHighest)
                .then(
                    if (current) {
                        Modifier.background(MaterialTheme.colorScheme.primary.copy(alpha = 0.08f))
                    } else {
                        Modifier
                    }
                )
                .clickable { listener.onSelect(card.id) },
            contentAlignment = Alignment.Center,
        ) {
            val thumbnail = state.thumbnails[card.id]
            if (thumbnail != null) {
                Image(
                    thumbnail.asImageBitmap(),
                    contentDescription = card.title,
                    contentScale = ContentScale.Crop,
                    alignment = Alignment.TopCenter,
                    modifier = Modifier.fillMaxSize(),
                )
            } else {
                Text(
                    card.host.ifEmpty { card.title },
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(16.dp),
                )
            }
        }
    }
}
