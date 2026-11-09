// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.compose.BackHandler
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner

/**
 * The extensions sheet: the window's extension actions (a tap runs one: its popup, or what it
 * does on click), and the way to the extensions page and the Chrome Web Store. Chrome's own
 * extensions menu lives in the toolbar Compass replaces.
 */
object ExtensionsUi {
    interface Listener {
        fun onRunAction(id: String)

        fun onManage()

        fun onWebStore()

        fun onDismiss()
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: ExtensionsState,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) { if (state.open) ExtensionsSheet(state, listener) }
        }
        return view
    }
}

@Composable
private fun ExtensionsSheet(state: ExtensionsState, listener: ExtensionsUi.Listener) {
    BackHandler { listener.onDismiss() }
    Box(Modifier.fillMaxSize()) {
        Box(
            Modifier.fillMaxSize()
                .background(Color.Black.copy(alpha = 0.32f))
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {
                    listener.onDismiss()
                }
        )
        Surface(
            color = MaterialTheme.colorScheme.surfaceContainerLow,
            shape = RoundedCornerShape(topStart = 28.dp, topEnd = 28.dp),
            modifier =
                Modifier.align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .heightIn(max = 560.dp)
                    // Taps on the sheet don't reach the scrim.
                    .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {},
        ) {
            Column(
                Modifier.fillMaxWidth().windowInsetsPadding(WindowInsets.navigationBars).padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("Extensions", style = MaterialTheme.typography.titleLarge)
                when {
                    !state.available ->
                        Text(
                            "Extensions aren't available in this window.",
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    state.actions.isEmpty() ->
                        Text(
                            "No extensions yet. Add them from the Chrome Web Store.",
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    else ->
                        LazyColumn(Modifier.heightIn(max = 400.dp)) {
                            items(state.actions, key = { it.id }) { action -> ActionRow(action, listener) }
                        }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextButton(onClick = { listener.onManage() }) { Text("Manage extensions") }
                    TextButton(onClick = { listener.onWebStore() }) { Text("Chrome Web Store") }
                }
            }
        }
    }
}

@Composable
private fun ActionRow(action: ExtensionsState.Action, listener: ExtensionsUi.Listener) {
    Row(
        Modifier.fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .clickable { listener.onRunAction(action.id) }
            .padding(horizontal = 8.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        val icon = action.icon
        if (icon != null) {
            Image(icon.asImageBitmap(), contentDescription = null, modifier = Modifier.size(28.dp))
        } else {
            Box(
                Modifier.size(28.dp).clip(RoundedCornerShape(6.dp)).background(MaterialTheme.colorScheme.surfaceContainerHighest)
            )
        }
        Column(Modifier.weight(1f)) {
            Text(action.name, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (action.title.isNotEmpty() && action.title != action.name) {
                Text(
                    action.title,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}
