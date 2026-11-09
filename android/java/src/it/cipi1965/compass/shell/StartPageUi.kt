// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner

/**
 * Compass's start page, in place of Chrome's new tab page (StartPage.java): the space, a search
 * field that opens the command bar, and the favorites and pinned entries to open.
 */
object StartPageUi {
    interface Listener {
        fun onSearch()

        fun onOpenNode(nodeId: String, url: String)

        /** The page's background, for Chrome (NativePage.getBackgroundColor()). */
        fun onBackgroundColor(color: Int)
    }

    @JvmStatic
    fun createView(
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
            CompassTheme(dark = isSystemInDarkTheme()) { StartPage(state, listener) }
        }
        return view
    }
}

@Composable
private fun StartPage(state: SidebarState, listener: StartPageUi.Listener) {
    val background = MaterialTheme.colorScheme.surface
    SideEffect { listener.onBackgroundColor(background.toArgb()) }
    val space = state.activeSpace
    val tint = space?.let { SidebarUi.tintColor(it.tint) } ?: MaterialTheme.colorScheme.outline
    // The pinned entries at the top of the space's tree (folders stay in the sidebar).
    val pinned = state.pinned.filter { !it.isFolder }
    Surface(color = background, modifier = Modifier.fillMaxSize()) {
        Column(
            modifier =
                Modifier.fillMaxSize()
                    .windowInsetsPadding(WindowInsets.statusBars)
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 24.dp, vertical = 32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Spacer(Modifier.height(48.dp))
            Box(
                Modifier.size(72.dp).clip(RoundedCornerShape(36.dp)).background(tint.copy(alpha = 0.25f)),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    space?.emoji?.ifEmpty { null } ?: space?.name?.take(1)?.uppercase() ?: "",
                    style = MaterialTheme.typography.headlineMedium,
                )
            }
            Text(
                space?.name ?: "",
                style = MaterialTheme.typography.headlineSmall,
                modifier = Modifier.padding(top = 12.dp),
            )
            Box(
                modifier =
                    Modifier.padding(top = 24.dp)
                        .widthIn(max = 560.dp)
                        .fillMaxWidth()
                        .height(52.dp)
                        .clip(RoundedCornerShape(26.dp))
                        .background(MaterialTheme.colorScheme.surfaceContainerHighest)
                        .clickable { listener.onSearch() }
                        .padding(horizontal = 20.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                Text(
                    "Search or type URL",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (state.favorites.isNotEmpty()) {
                Section("Favorites", state.favorites, listener)
            }
            if (pinned.isNotEmpty()) {
                Section("Pinned", pinned, listener)
            }
            if (state.loaded && state.favorites.isEmpty() && pinned.isEmpty()) {
                Text(
                    "Pin tabs and add favorites from the sidebar to find them here.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.padding(top = 32.dp).widthIn(max = 360.dp),
                )
            }
            // Room for the bottom bar.
            Spacer(Modifier.height(96.dp))
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun Section(
    title: String,
    nodes: List<SidebarState.Node>,
    listener: StartPageUi.Listener,
) {
    Text(
        title,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(top = 32.dp, bottom = 12.dp).widthIn(max = 560.dp).fillMaxWidth(),
    )
    FlowRow(
        modifier = Modifier.widthIn(max = 560.dp).fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        for (node in nodes) {
            Column(
                Modifier.width(72.dp).clip(RoundedCornerShape(12.dp)).clickable {
                    listener.onOpenNode(node.id, node.url)
                },
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Box(
                    Modifier.size(56.dp)
                        .clip(RoundedCornerShape(16.dp))
                        .background(MaterialTheme.colorScheme.surfaceContainerHighest),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(initialOf(node), style = MaterialTheme.typography.titleLarge)
                }
                Text(
                    node.title.ifEmpty { node.url },
                    style = MaterialTheme.typography.labelMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(top = 6.dp),
                )
            }
        }
    }
}

private fun initialOf(node: SidebarState.Node): String {
    val text = node.title.ifEmpty { node.url.substringAfter("://").removePrefix("www.") }
    return text.take(1).uppercase().ifEmpty { "•" }
}
