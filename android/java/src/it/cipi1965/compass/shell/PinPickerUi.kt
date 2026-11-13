// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R

/**
 * Pin to Compass's picker (the share target, docs/design/android.md, P9), as the iOS share
 * extension's: the shared page is pinned to the active space, and this sheet moves it to another
 * space's pinned tree, one of its folders, or the favorites.
 */
object PinPickerUi {
    interface Listener {
        /** Moves the pinned entry into |parentId|: a folder, a space's pinned root, or the favorites'. */
        fun onPinInto(parentId: String)

        fun onPinPickerDone()
    }

    class State {
        var open by mutableStateOf(false)
        /** The pinned page's title, or its URL. */
        var title by mutableStateOf("")
        /** Where it is now: a folder's ID or a root's (its space's pinned tree, the favorites'). */
        var parentId by mutableStateOf("")
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        sidebar: SidebarState,
        state: State,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) {
                CompassSheet(state.open, onDismiss = { listener.onPinPickerDone() }) {
                    Row(
                        Modifier.fillMaxWidth().padding(start = 24.dp, end = 12.dp, bottom = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column(Modifier.weight(1f)) {
                            Text("Pinned to Compass", style = MaterialTheme.typography.titleLarge)
                            Text(
                                state.title,
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                        TextButton(onClick = { listener.onPinPickerDone() }) { Text("Done") }
                    }
                    LazyColumn(
                        Modifier.fillMaxWidth().heightIn(max = 560.dp),
                        contentPadding = PaddingValues(bottom = 24.dp),
                    ) {
                        item {
                            Place("Favorites", R.drawable.compass_sym_star, 0, state.parentId == sidebar.favoritesId) {
                                listener.onPinInto(sidebar.favoritesId)
                            }
                        }
                        for (space in sidebar.spaces) {
                            if (space.pinnedId.isEmpty()) continue
                            item(key = space.id) {
                                Text(
                                    listOf(space.emoji, space.name.ifEmpty { "Space" }).filter { it.isNotEmpty() }.joinToString(" "),
                                    style = MaterialTheme.typography.titleSmall,
                                    color = MaterialTheme.colorScheme.primary,
                                    modifier = Modifier.padding(start = 24.dp, top = 16.dp, bottom = 4.dp),
                                )
                            }
                            item(key = space.pinnedId) {
                                Place("Pinned", R.drawable.compass_sym_keep, 0, state.parentId == space.pinnedId) {
                                    listener.onPinInto(space.pinnedId)
                                }
                            }
                            items(space.folders, key = { it.id }) { folder ->
                                Place(
                                    folder.title.ifEmpty { "Folder" },
                                    R.drawable.compass_sym_folder,
                                    folder.depth + 1,
                                    state.parentId == folder.id,
                                ) {
                                    listener.onPinInto(folder.id)
                                }
                            }
                        }
                    }
                }
            }
        }
        return view
    }
}

/** A place to pin to, indented by its depth; the one it's in is checked. */
@androidx.compose.runtime.Composable
private fun Place(title: String, icon: Int, depth: Int, current: Boolean, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth()
            .heightIn(min = 52.dp)
            .clickable(onClick = onClick)
            .padding(start = (16 + 20 * depth).dp, end = 24.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        MenuIcon(icon)
        Text(
            title,
            style = MaterialTheme.typography.bodyLarge,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f),
        )
        if (current) Sym(R.drawable.compass_sym_check, 18f, MaterialTheme.colorScheme.primary)
    }
}
