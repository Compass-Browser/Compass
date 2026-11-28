// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.annotation.DrawableRes
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
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
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R
import kotlinx.coroutines.delay

/**
 * The page menu, as the iOS app's (PageMenu.swift), in place of Chrome's app menu: the address
 * with back, forward, copy and reload; Find, Ask Claude, Pin and Share; Downloads and History; the
 * window's extensions; Site Settings and Settings. A sheet over the page, in the space's tint.
 */
object PageMenuUi {
    interface Listener {
        fun onBack()

        fun onForward()

        fun onEditAddress()

        fun onCopyUrl()

        fun onReload()

        fun onFind()

        fun onReader()

        fun onTranslate()

        fun onAskClaude()

        fun onCaptureToEasel()

        fun onTogglePin()

        fun onShare()

        fun onDownloads()

        fun onHistory()

        fun onRunExtension(id: String)

        fun onExtensions()

        fun onSiteSettings()

        fun onSettings()

        fun onDismiss()
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: PageMenuState,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme() || state.incognito) {
                CompassSheet(state.open, onDismiss = { listener.onDismiss() }) { PageMenuSheet(state, listener) }
            }
        }
        return view
    }
}

@Composable
private fun PageMenuSheet(state: PageMenuState, listener: PageMenuUi.Listener) {
    Column(
        Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        AddressCard(state, listener)
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Tile(stringResource(R.string.compass_menu_find), R.drawable.compass_sym_find_in_page) { listener.onFind() }
            // Reader: on any web page, as on the desktop (one without an article says so).
            Tile(
                stringResource(R.string.compass_menu_reader),
                R.drawable.compass_sym_menu_book,
                enabled = state.url.startsWith("http"),
                selected = state.inReader,
            ) {
                listener.onReader()
            }
            // Page translation (docs/design/translation.md, "Android"): Chrome's own, on Compass's
            // engine and models.
            Tile(
                stringResource(R.string.compass_menu_translate),
                R.drawable.compass_sym_translate,
                enabled = state.canTranslate,
            ) {
                listener.onTranslate()
            }
            if (!state.incognito) Tile(state.askTitle, R.drawable.compass_sym_auto_awesome) { listener.onAskClaude() }
            Tile(
                stringResource(if (state.pinned) R.string.compass_menu_unpin else R.string.compass_menu_pin),
                if (state.pinned) R.drawable.compass_sym_keep_off else R.drawable.compass_sym_keep,
                enabled = !state.incognito && state.url.isNotEmpty(),
                selected = state.pinned,
            ) {
                listener.onTogglePin()
            }
            Tile(stringResource(R.string.compass_share), R.drawable.compass_sym_share, enabled = state.url.isNotEmpty()) { listener.onShare() }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            WideButton(stringResource(R.string.compass_menu_downloads), R.drawable.compass_sym_download) { listener.onDownloads() }
            WideButton(stringResource(R.string.compass_menu_history), R.drawable.compass_sym_history) { listener.onHistory() }
        }
        ExtensionsRow(state, listener)
        Card {
            // Easels (docs/design/android.md, "Easels"): the visible page, to a board.
            if (!state.incognito && state.url.startsWith("http")) {
                CardRow(
                    stringResource(R.string.compass_menu_capture_to_easel),
                    subtitle = stringResource(R.string.compass_menu_capture_to_easel_subtitle),
                    first = true,
                    onClick = { listener.onCaptureToEasel() },
                ) {
                    MenuIcon(R.drawable.compass_sym_easel)
                }
            }
            CardRow(
                stringResource(R.string.compass_menu_site_settings),
                subtitle = state.host.removePrefix("www.").ifEmpty { stringResource(R.string.compass_menu_no_page) },
                first = state.incognito || !state.url.startsWith("http"),
                onClick = { listener.onSiteSettings() },
            ) {
                MenuIcon(R.drawable.compass_sym_tune)
            }
            CardRow(
                stringResource(R.string.compass_settings),
                subtitle = stringResource(R.string.compass_menu_settings_subtitle),
                onClick = { listener.onSettings() },
            ) {
                MenuIcon(R.drawable.compass_sym_settings_fill)
            }
        }
    }
}

/** Back, forward, the host (tap: edit it), copy and reload, in one capsule. */
@Composable
private fun AddressCard(state: PageMenuState, listener: PageMenuUi.Listener) {
    var copied by remember { mutableStateOf(false) }
    LaunchedEffect(copied) {
        if (copied) {
            delay(1200)
            copied = false
        }
    }
    Row(
        Modifier.fillMaxWidth().height(56.dp).clip(CircleShape).background(CompassStyle.cardFill()).padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        CardIcon(R.drawable.compass_sym_arrow_back, state.canGoBack) { listener.onBack() }
        CardIcon(R.drawable.compass_sym_arrow_forward, state.canGoForward) { listener.onForward() }
        Text(
            state.host.removePrefix("www.").ifEmpty { stringResource(R.string.compass_bar_search_or_enter_address) },
            style = MaterialTheme.typography.bodyLarge,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier =
                Modifier.weight(1f)
                    .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {
                        listener.onEditAddress()
                    }
                    .padding(horizontal = 6.dp, vertical = 12.dp),
        )
        CardIcon(if (copied) R.drawable.compass_sym_check else R.drawable.compass_sym_link, state.url.isNotEmpty()) {
            listener.onCopyUrl()
            copied = true
        }
        CardIcon(R.drawable.compass_sym_refresh, state.url.isNotEmpty()) { listener.onReload() }
    }
}

@Composable
private fun CardIcon(@DrawableRes icon: Int, enabled: Boolean, onClick: () -> Unit) {
    Box(
        Modifier.size(44.dp).clip(CircleShape).clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Sym(icon, 18f, modifier = Modifier.alpha(if (enabled) 1f else 0.3f))
    }
}

/** A square action: its glyph over its label, on a tonal container (primary when selected). */
@Composable
private fun RowScope.Tile(
    label: String,
    @DrawableRes icon: Int,
    enabled: Boolean = true,
    selected: Boolean = false,
    onClick: () -> Unit,
) {
    val container = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.secondaryContainer
    val content = if (selected) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSecondaryContainer
    Column(
        Modifier.weight(1f)
            .height(84.dp)
            .clip(RoundedCornerShape(20.dp))
            .background(container)
            .clickable(enabled = enabled, onClick = onClick)
            .alpha(if (enabled) 1f else 0.38f),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterVertically),
    ) {
        Sym(icon, 20f, content)
        Text(label, style = MaterialTheme.typography.labelMedium, color = content, maxLines = 1)
    }
}

/** A wide action: Material's filled tonal button. */
@Composable
private fun RowScope.WideButton(label: String, @DrawableRes icon: Int, onClick: () -> Unit) {
    Row(
        Modifier.weight(1f)
            .height(48.dp)
            .clip(CircleShape)
            .background(MaterialTheme.colorScheme.secondaryContainer)
            .clickable(onClick = onClick),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
    ) {
        Sym(icon, 15f, MaterialTheme.colorScheme.onSecondaryContainer)
        Text(label, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSecondaryContainer, maxLines = 1)
    }
}

/** The window's extensions as round buttons (a tap runs one), then the extensions sheet. */
@Composable
private fun ExtensionsRow(state: PageMenuState, listener: PageMenuUi.Listener) {
    if (state.extensions.isEmpty()) return
    LazyRow(horizontalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.height(54.dp)) {
        items(state.extensions, key = { it.id }) { action ->
            Box(
                Modifier.size(50.dp).clip(CircleShape).background(CompassStyle.cardFill()).clickable {
                    listener.onRunExtension(action.id)
                },
                contentAlignment = Alignment.Center,
            ) {
                val icon = action.icon
                if (icon != null) {
                    Image(icon.asImageBitmap(), contentDescription = action.name, modifier = Modifier.size(28.dp))
                } else {
                    Sym(R.drawable.compass_sym_extension_fill, 20f)
                }
            }
        }
        item {
            Box(
                Modifier.size(50.dp).clip(CircleShape).background(CompassStyle.cardFill()).clickable { listener.onExtensions() },
                contentAlignment = Alignment.Center,
            ) {
                Sym(R.drawable.compass_sym_extension_fill, 20f, MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f))
            }
        }
    }
}
