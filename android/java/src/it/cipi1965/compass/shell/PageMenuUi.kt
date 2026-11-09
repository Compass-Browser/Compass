// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.compose.BackHandler
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.annotation.DrawableRes
import androidx.compose.animation.core.Animatable
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R
import kotlin.math.roundToInt
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

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

        fun onAskClaude()

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
                if (state.open) PageMenuSheet(state, listener)
            }
        }
        return view
    }
}

@Composable
private fun PageMenuSheet(state: PageMenuState, listener: PageMenuUi.Listener) {
    BackHandler { listener.onDismiss() }
    val scope = rememberCoroutineScope()
    // A drag down on the sheet puts it away.
    val drag = remember { Animatable(0f) }
    val dismissPx = with(LocalDensity.current) { 110.dp.toPx() }
    Box(Modifier.fillMaxSize()) {
        Box(
            Modifier.fillMaxSize()
                .background(Color.Black.copy(alpha = 0.2f))
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {
                    listener.onDismiss()
                }
        )
        Column(
            Modifier.align(Alignment.BottomCenter)
                .fillMaxWidth()
                .offset { IntOffset(0, drag.value.roundToInt()) }
                .clip(RoundedCornerShape(topStart = 28.dp, topEnd = 28.dp))
                .spaceTint(if (state.incognito) "" else state.tint, MaterialTheme.colorScheme.surfaceContainerLow)
                // Taps on the sheet don't reach the scrim.
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {}
                .pointerInput(Unit) {
                    detectVerticalDragGestures(
                        onDragEnd = {
                            scope.launch {
                                if (drag.value > dismissPx) listener.onDismiss() else drag.animateTo(0f)
                            }
                        },
                        onDragCancel = { scope.launch { drag.animateTo(0f) } },
                    ) { change, dy ->
                        change.consume()
                        scope.launch { drag.snapTo((drag.value + dy).coerceAtLeast(0f)) }
                    }
                }
                .windowInsetsPadding(WindowInsets.navigationBars)
                .padding(start = 14.dp, end = 14.dp, bottom = 14.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Grabber()
            AddressCard(state, listener)
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Tile("Find", R.drawable.compass_sym_find_in_page) { listener.onFind() }
                if (!state.incognito) Tile("Ask Claude", R.drawable.compass_sym_auto_awesome) { listener.onAskClaude() }
                Tile(
                    if (state.pinned) "Unpin" else "Pin",
                    if (state.pinned) R.drawable.compass_sym_keep_off else R.drawable.compass_sym_keep,
                    enabled = !state.incognito && state.url.isNotEmpty(),
                    tint = if (state.pinned) CompassStyle.accent(state.tint) else null,
                ) {
                    listener.onTogglePin()
                }
                Tile("Share", R.drawable.compass_sym_ios_share, enabled = state.url.isNotEmpty()) { listener.onShare() }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                WideButton("Downloads", R.drawable.compass_sym_download) { listener.onDownloads() }
                WideButton("History", R.drawable.compass_sym_history) { listener.onHistory() }
            }
            ExtensionsRow(state, listener)
            Card {
                CardRow(
                    "Site Settings",
                    subtitle = state.host.removePrefix("www.").ifEmpty { "No page" },
                    first = true,
                    trailing = R.drawable.compass_sym_chevron_right,
                    onClick = { listener.onSiteSettings() },
                ) {
                    MenuIcon(R.drawable.compass_sym_tune, CompassStyle.blue)
                }
                CardRow(
                    "Settings",
                    subtitle = "Sync, passwords, search engine",
                    trailing = R.drawable.compass_sym_chevron_right,
                    onClick = { listener.onSettings() },
                ) {
                    MenuIcon(R.drawable.compass_sym_settings_fill, CompassStyle.gray)
                }
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
        CardIcon(R.drawable.compass_sym_arrow_back_ios_new, state.canGoBack) { listener.onBack() }
        CardIcon(R.drawable.compass_sym_arrow_forward_ios, state.canGoForward) { listener.onForward() }
        Text(
            state.host.removePrefix("www.").ifEmpty { "Search or enter address" },
            fontSize = 16.sp,
            fontWeight = FontWeight.Medium,
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

/** A square action: its glyph over its label, on a card (PageMenu.swift, tile). */
@Composable
private fun RowScope.Tile(
    label: String,
    @DrawableRes icon: Int,
    enabled: Boolean = true,
    tint: Color? = null,
    onClick: () -> Unit,
) {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val color = tint ?: MaterialTheme.colorScheme.onSurface
    Column(
        Modifier.weight(1f)
            .height(84.dp)
            .scale(if (pressed) 0.96f else 1f)
            .clip(RoundedCornerShape(18.dp))
            .background(CompassStyle.cardFill())
            .clickable(interactionSource = interaction, indication = null, enabled = enabled, onClick = onClick)
            .alpha(if (enabled) 1f else 0.35f),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterVertically),
    ) {
        Sym(icon, 22f, color)
        Text(label, fontSize = 12.sp, fontWeight = FontWeight.Medium, color = color, maxLines = 1)
    }
}

/** A wide capsule action (PageMenu.swift, Downloads and Archive). */
@Composable
private fun RowScope.WideButton(label: String, @DrawableRes icon: Int, onClick: () -> Unit) {
    Row(
        Modifier.weight(1f).height(50.dp).clip(CircleShape).background(CompassStyle.cardFill()).clickable(onClick = onClick),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
    ) {
        Sym(icon, 17f)
        Text(label, fontSize = 17.sp, fontWeight = FontWeight.Medium, maxLines = 1)
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
