// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.graphics.Rect
import android.view.View
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner

/**
 * A split's chrome (SplitView): each pane's title strip and the front pane's outline, in the
 * space's accent; or, for three or four panes, the pane picker. Only the strips and the picker
 * take touches: the pages under the rest get them.
 */
object SplitUi {
    enum class Mode { NONE, TOGETHER, PICKER }

    /** A pane: its entry, and where it is in the window (px; empty in the picker). */
    class Pane(
        @JvmField val id: String,
        @JvmField val title: String,
        @JvmField val url: String,
        @JvmField val front: Boolean,
        @JvmField val rect: Rect,
    )

    interface Listener {
        /** A tap on a pane's strip, or in the picker: that pane comes to the front. */
        fun onFront(paneId: String)
    }

    class State {
        var mode by mutableStateOf(Mode.NONE)
        var panes by mutableStateOf<List<Pane>>(emptyList())
        var stripPx by mutableIntStateOf(0)
        var pickerPx by mutableIntStateOf(0)
        /** The space's accent (ARGB); 0: the theme's primary. */
        var accent by mutableIntStateOf(0)

        fun show(mode: Mode, panes: List<Pane>, stripPx: Int, pickerPx: Int) {
            this.mode = mode
            this.panes = panes
            this.stripPx = stripPx
            this.pickerPx = pickerPx
        }

        fun hide() {
            mode = Mode.NONE
            panes = emptyList()
        }
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        state: State,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setContent { CompassTheme(dark = isSystemInDarkTheme()) { SplitOverlay(state, listener) } }
        return view
    }
}

@Composable
private fun SplitOverlay(state: SplitUi.State, listener: SplitUi.Listener) {
    val accent = if (state.accent != 0) Color(state.accent) else MaterialTheme.colorScheme.primary
    when (state.mode) {
        SplitUi.Mode.NONE -> {}
        SplitUi.Mode.TOGETHER -> Box(Modifier.fillMaxSize()) {
            for (pane in state.panes) PaneFrame(pane, state.stripPx, accent, listener)
        }
        // In a box: the view's own constraints are exact, which a height alone can't change.
        SplitUi.Mode.PICKER -> Box(Modifier.fillMaxSize()) { PanePicker(state, accent, listener) }
    }
}

/** A pane of a split of two: its strip, and the outline when it's in front. */
@Composable
private fun PaneFrame(pane: SplitUi.Pane, stripPx: Int, accent: Color, listener: SplitUi.Listener) {
    val density = LocalDensity.current
    val r = pane.rect
    Box(
        Modifier.offset { IntOffset(r.left, r.top) }
            .size(with(density) { r.width().toDp() }, with(density) { r.height().toDp() })
            .then(if (pane.front) Modifier.border(2.dp, accent) else Modifier)
    ) {
        Row(
            Modifier.fillMaxWidth()
                .height(with(density) { stripPx.toDp() })
                .background(
                    if (pane.front) accent.copy(alpha = 0.18f).compositeOver(MaterialTheme.colorScheme.surface)
                    else MaterialTheme.colorScheme.surfaceContainerHigh
                )
                .clickable { if (!pane.front) listener.onFront(pane.id) }
                .padding(horizontal = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            SiteIcon(pane.url, 16.dp)
            Text(
                pane.title.ifEmpty { pane.url }.ifEmpty { "Untitled" },
                style = MaterialTheme.typography.labelLarge,
                fontWeight = if (pane.front) FontWeight.SemiBold else FontWeight.Normal,
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = if (pane.front) 1f else 0.7f),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

/** A split of three or four: one segment per pane, the shown one selected. */
@Composable
private fun PanePicker(state: SplitUi.State, accent: Color, listener: SplitUi.Listener) {
    val density = LocalDensity.current
    Box(
        Modifier.fillMaxWidth()
            .height(with(density) { state.pickerPx.toDp() })
            .background(MaterialTheme.colorScheme.surface)
            .padding(horizontal = 12.dp, vertical = 8.dp)
    ) {
        Row(
            Modifier.fillMaxSize()
                .clip(RoundedCornerShape(50))
                .background(MaterialTheme.colorScheme.surfaceContainerHighest)
                .padding(3.dp),
            horizontalArrangement = Arrangement.spacedBy(3.dp),
        ) {
            for (pane in state.panes) {
                Row(
                    Modifier.weight(1f)
                        .fillMaxHeight()
                        .clip(RoundedCornerShape(50))
                        .background(if (pane.front) MaterialTheme.colorScheme.surface else Color.Transparent)
                        .then(if (pane.front) Modifier.border(1.5.dp, accent, RoundedCornerShape(50)) else Modifier)
                        .clickable { if (!pane.front) listener.onFront(pane.id) }
                        .padding(horizontal = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp, Alignment.CenterHorizontally),
                ) {
                    SiteIcon(pane.url, 14.dp)
                    Text(
                        pane.title.ifEmpty { "Untitled" },
                        style = MaterialTheme.typography.labelMedium,
                        fontWeight = if (pane.front) FontWeight.SemiBold else FontWeight.Normal,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }
    }
}

private fun Color.compositeOver(background: Color): Color {
    val a = alpha
    return Color(
        red * a + background.red * (1 - a),
        green * a + background.green * (1 - a),
        blue * a + background.blue * (1 - a),
    )
}
