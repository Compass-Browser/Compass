// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.os.Build
import android.view.View
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner

object CompassBarUi {
    /** What the bar's controls do. */
    interface Actions {
        fun showSidebar()

        fun openAddress()

        fun showTabs()

        fun newTab()

        fun showMenu()

        /** The bar's background, for the strip under the navigation bar. */
        fun backgroundColorChanged(color: Int)
    }

    /** The bar as a view for Chrome's layout. */
    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        state: CompassBarState,
        actions: Actions,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setContent {
            CompassTheme(dark = state.incognito || isSystemInDarkTheme()) {
                // Chrome paints the strip under the navigation bar in the bar's color.
                val background = MaterialTheme.colorScheme.surfaceContainer.toArgb()
                SideEffect { actions.backgroundColorChanged(background) }
                Bar(state, actions)
            }
        }
        return view
    }
}

@Composable
internal fun CompassTheme(dark: Boolean, content: @Composable () -> Unit) {
    val context = LocalContext.current
    val colors =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        } else {
            if (dark) darkColorScheme() else lightColorScheme()
        }
    MaterialTheme(colorScheme = colors, content = content)
}

@Composable
private fun Bar(state: CompassBarState, actions: CompassBarUi.Actions) {
    Surface(color = MaterialTheme.colorScheme.surfaceContainer, modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier.fillMaxSize().padding(horizontal = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            SpaceButton(state) { actions.showSidebar() }
            Box(
                modifier =
                    Modifier.weight(1f)
                        .height(44.dp)
                        .clip(RoundedCornerShape(22.dp))
                        .background(MaterialTheme.colorScheme.surfaceContainerHighest)
                        .clickable { actions.openAddress() }
                        .padding(horizontal = 16.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                val empty = state.host.isEmpty()
                Text(
                    text = if (empty) "Search or type URL" else state.host,
                    style = MaterialTheme.typography.bodyLarge,
                    color =
                        if (empty) MaterialTheme.colorScheme.onSurfaceVariant
                        else MaterialTheme.colorScheme.onSurface,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            TabCount(state.tabCount) { actions.showTabs() }
            BarButton("+", true) { actions.newTab() }
            BarButton("⋮", true) { actions.showMenu() }
        }
    }
}

@Composable
private fun SpaceButton(state: CompassBarState, onClick: () -> Unit) {
    val tint = SidebarUi.tintColor(state.spaceTint) ?: MaterialTheme.colorScheme.outline
    Box(
        modifier =
            Modifier.size(48.dp).clip(RoundedCornerShape(24.dp)).clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Box(
            Modifier.size(34.dp).clip(RoundedCornerShape(17.dp)).background(tint.copy(alpha = 0.25f)),
            contentAlignment = Alignment.Center,
        ) {
            Text(state.spaceLabel.ifEmpty { "☰" }, style = MaterialTheme.typography.titleMedium)
        }
    }
}

@Composable
private fun BarButton(label: String, enabled: Boolean, onClick: () -> Unit) {
    TextButton(
        onClick = onClick,
        enabled = enabled,
        modifier = Modifier.size(48.dp),
        contentPadding = PaddingValues(0.dp),
    ) {
        Text(label, style = MaterialTheme.typography.headlineSmall)
    }
}

@Composable
private fun TabCount(count: Int, onClick: () -> Unit) {
    Box(
        modifier =
            Modifier.clip(RoundedCornerShape(12.dp))
                .clickable(onClick = onClick)
                .padding(11.dp)
                .size(26.dp)
                .border(2.dp, MaterialTheme.colorScheme.onSurface, RoundedCornerShape(7.dp)),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            if (count > 99) "99+" else count.toString(),
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurface,
        )
    }
}
