// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.graphics.Bitmap
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.annotation.DrawableRes
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R

/**
 * A link's (or an image's, or the page's) long-press menu, Compass's: a sheet with the link at the
 * top and its items in cards, each with its icon, as iOS's context menu (docs/design/android.md,
 * "The link menu"). Chrome builds the items and runs them; Compass adds its own.
 */
object LinkMenuUi {
    interface Listener {
        fun onItem(key: Int)

        fun onSubmenuBack()

        fun onDismiss()
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: LinkMenuState,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) {
                CompassSheet(state.open, onDismiss = { listener.onDismiss() }) { LinkMenuSheet(state, listener) }
            }
        }
        return view
    }
}

@Composable
private fun LinkMenuSheet(state: LinkMenuState, listener: LinkMenuUi.Listener) {
    Column(
        Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(start = 16.dp, end = 16.dp, bottom = 16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Header(state)
        if (state.submenuTitle.isNotEmpty()) {
            Card {
                ItemRow(state.submenuTitle, R.drawable.compass_sym_arrow_back, enabled = true) { listener.onSubmenuBack() }
            }
        }
        for (group in state.groups) {
            if (group.isEmpty()) continue
            Card {
                for (item in group) {
                    ItemRow(
                        item.title,
                        item.icon,
                        item.enabled,
                        bitmap = item.bitmap,
                        trailing = if (item.submenu) R.drawable.compass_sym_chevron_right else null,
                    ) {
                        listener.onItem(item.key)
                    }
                }
            }
        }
    }
}

/** The link: an image's thumbnail or the site's icon, its text and its address. */
@Composable
private fun Header(state: LinkMenuState) {
    if (state.title.isEmpty() && state.url.isEmpty() && state.image == null) return
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        val image = state.image
        if (image != null) {
            Image(
                image.asImageBitmap(),
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier.size(56.dp).clip(RoundedCornerShape(12.dp)),
            )
        } else if (state.url.isNotEmpty()) {
            SiteTile(state.url)
        }
        Column(Modifier.weight(1f)) {
            if (state.title.isNotEmpty()) {
                Text(
                    state.title,
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            if (state.url.isNotEmpty()) {
                Text(
                    state.url,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

@Composable
private fun ItemRow(
    title: String,
    @DrawableRes icon: Int,
    enabled: Boolean,
    bitmap: Bitmap? = null,
    @DrawableRes trailing: Int? = null,
    onClick: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth()
            .heightIn(min = 52.dp)
            .then(if (enabled) Modifier.clickable(onClick = onClick) else Modifier.alpha(0.38f))
            .padding(horizontal = 12.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        when {
            bitmap != null ->
                Box(Modifier.size(40.dp), contentAlignment = Alignment.Center) {
                    Image(bitmap.asImageBitmap(), contentDescription = null, modifier = Modifier.size(20.dp))
                }
            icon != 0 -> MenuIcon(icon)
            else -> Box(Modifier.size(40.dp))
        }
        Text(
            title,
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f),
        )
        if (trailing != null) Sym(trailing, 16f, MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
