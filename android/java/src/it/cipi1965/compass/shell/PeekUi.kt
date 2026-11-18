// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import android.view.ViewGroup
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.compose.BackHandler
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.MutableTransitionState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.Orientation
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.draggable
import androidx.compose.foundation.gestures.rememberDraggableState
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
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
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R
import kotlin.math.roundToInt
import kotlinx.coroutines.launch

/**
 * Peek's card (Peek.java), as the iOS app's (Peek.swift) in Material's style: the page in a card
 * over the dimmed window, up from the bottom on a phone and centered in a wide window. Its bar has
 * the page's title and host, Open and close; dragging the bar down, back, or a tap on the dim
 * closes it. In little-window mode the bar has close, back, the address (tap to edit) and Open in
 * <space>, with the window's profile's other spaces under ⌄.
 */
object PeekUi {
    /** How long the card takes to slide away. */
    const val CLOSE_MS = 220L

    interface Listener {
        fun onClose()

        /** Open: the page becomes a tab of |spaceId| (empty: the space shown). */
        fun onOpen(spaceId: String)

        fun onBack()

        fun onReload()

        /** The little window's address: start or stop editing it. */
        fun onEditAddress(editing: Boolean)

        fun onAddressChanged(text: String)

        fun onGo(text: String)

        /** Always Open <domain> in <space>: the domain's rule, then Open there. */
        fun onAlwaysOpenIn(spaceId: String)
    }

    /** A space of the window's profile, for Open in <space>'s menu. */
    class SpaceRow(
        @JvmField val id: String,
        @JvmField val name: String,
        @JvmField val emoji: String,
        @JvmField val tint: String,
    )

    class State {
        var open by mutableStateOf(false)
        /** A link from another app (android.md, "Little windows"). */
        var little by mutableStateOf(false)
        var title by mutableStateOf("")
        var host by mutableStateOf("")
        var url by mutableStateOf("")
        var canGoBack by mutableStateOf(false)
        var loading by mutableStateOf(false)
        /** The little window's address is being edited, and what's typed. */
        var editing by mutableStateOf(false)
        var address by mutableStateOf("")
        /** The space Open opens into, and the window's profile's spaces. */
        var spaceId by mutableStateOf("")
        var spaceName by mutableStateOf("")
        var spaces by mutableStateOf<List<SpaceRow>>(emptyList())
        /** The page's domain as a link rule's (empty: none), and the space its rule names. */
        var ruleDomain by mutableStateOf("")
        var ruleSpaceId by mutableStateOf("")
        /** The shown space's theme, which tints the little window's bar. */
        var theme by mutableStateOf<SpaceTheme?>(null)
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: State,
        pageHost: View,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) { PeekOverlay(state, pageHost, listener) }
        }
        return view
    }
}

@Composable
private fun PeekOverlay(state: PeekUi.State, pageHost: View, listener: PeekUi.Listener) {
    BackHandler(enabled = state.open) {
        if (state.editing) listener.onEditAddress(false) else listener.onClose()
    }
    val dim by animateFloatAsState(
        if (state.open) 0.45f else 0f,
        tween(if (state.open) 200 else PeekUi.CLOSE_MS.toInt()),
        label = "dim",
    )
    val shown = remember { MutableTransitionState(false) }
    shown.targetState = state.open
    BoxWithConstraints(Modifier.fillMaxSize()) {
        // The dimmed window: a tap closes the card.
        Box(
            Modifier.fillMaxSize()
                .background(Color.Black.copy(alpha = dim))
                .then(
                    if (state.open) Modifier.pointerInput(Unit) { detectTapGestures { listener.onClose() } }
                    else Modifier
                )
        )
        val wide = maxWidth >= 840.dp
        val drag = remember { Animatable(0f) }
        val scope = rememberCoroutineScope()
        val closeAt = with(LocalDensity.current) { 120.dp.toPx() }
        LaunchedEffect(state.open) { if (state.open) drag.snapTo(0f) }
        AnimatedVisibility(
            visibleState = shown,
            modifier = Modifier.align(if (wide) Alignment.Center else Alignment.BottomCenter),
            enter =
                if (wide) fadeIn(tween(160)) + scaleIn(tween(160), initialScale = 0.92f)
                else slideInVertically(tween(260)) { it } + fadeIn(tween(120)),
            exit =
                if (wide) fadeOut(tween(PeekUi.CLOSE_MS.toInt())) + scaleOut(tween(PeekUi.CLOSE_MS.toInt()), targetScale = 0.92f)
                else slideOutVertically(tween(PeekUi.CLOSE_MS.toInt())) { it } + fadeOut(tween(PeekUi.CLOSE_MS.toInt())),
        ) {
            val shape = if (wide) RoundedCornerShape(28.dp) else RoundedCornerShape(topStart = 28.dp, topEnd = 28.dp)
            val size =
                if (wide) Modifier.widthIn(max = 760.dp).fillMaxWidth(0.9f).fillMaxHeight(0.9f)
                else Modifier.fillMaxWidth().fillMaxHeight().windowInsetsPadding(WindowInsets.statusBars).padding(top = 12.dp)
            // Dragging the bar down closes the card, as a sheet's.
            val dragBar =
                Modifier.draggable(
                    orientation = Orientation.Vertical,
                    state = rememberDraggableState { delta -> scope.launch { drag.snapTo((drag.value + delta).coerceAtLeast(0f)) } },
                    onDragStopped = { velocity ->
                        if (drag.value > closeAt || velocity > 1800f) listener.onClose()
                        else drag.animateTo(0f, tween(180))
                    },
                )
            Column(
                size.offset { IntOffset(0, drag.value.roundToInt()) }
                    .shadow(12.dp, shape)
                    .clip(shape)
                    .background(MaterialTheme.colorScheme.surfaceContainerLow)
                    // Taps on the card aren't the dim's.
                    .pointerInput(Unit) { detectTapGestures { } }
            ) {
                if (state.little) {
                    SpaceThemed(state.theme) { LittleBar(state, listener, dragBar) }
                } else {
                    PeekBar(state, listener, dragBar)
                }
                Box(Modifier.fillMaxWidth().height(2.dp)) {
                    if (state.loading) LinearProgressIndicator(Modifier.fillMaxSize())
                }
                AndroidView(
                    factory = {
                        (pageHost.parent as? ViewGroup)?.removeView(pageHost)
                        pageHost
                    },
                    modifier =
                        Modifier.fillMaxWidth().weight(1f).then(
                            if (wide) Modifier else Modifier.windowInsetsPadding(WindowInsets.navigationBars)
                        ),
                )
            }
        }
    }
}

/** Peek's bar: the page's icon, title and host, Open and close. */
@Composable
private fun PeekBar(state: PeekUi.State, listener: PeekUi.Listener, dragBar: Modifier) {
    Column(dragBar.fillMaxWidth()) {
        Grabber()
        Row(
            Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, bottom = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            SiteIcon(state.url, 20.dp)
            Column(Modifier.weight(1f)) {
                // Only the host until the page has a title.
                val title = state.title.takeIf { it.isNotEmpty() && it != state.url } ?: state.host
                Text(title, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (title != state.host && state.host.isNotEmpty()) {
                    Text(
                        state.host.removePrefix("www."),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                    )
                }
            }
            FilledTonalButton(onClick = { listener.onOpen("") }) { Text("Open") }
            IconCircle(R.drawable.compass_sym_close) { listener.onClose() }
        }
    }
}

/** The little window's bar: close, back, the address, and Open in <space> with its ⌄ menu. */
@Suppress("DEPRECATION")
@Composable
private fun LittleBar(state: PeekUi.State, listener: PeekUi.Listener, dragBar: Modifier) {
    Column(dragBar.fillMaxWidth().spaceTint(state.theme) { it.surfaceContainerLow }) {
        Grabber()
        Row(
            Modifier.fillMaxWidth().padding(start = 4.dp, end = 8.dp, bottom = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            IconCircle(R.drawable.compass_sym_close) { listener.onClose() }
            IconCircle(R.drawable.compass_sym_arrow_back, enabled = state.canGoBack) { listener.onBack() }
            // The address: the title and host; a tap edits the URL.
            Box(
                Modifier.weight(1f)
                    .height(40.dp)
                    .clip(CircleShape)
                    .background(MaterialTheme.colorScheme.onSurface.copy(alpha = 0.07f))
                    .clickable(enabled = !state.editing) { listener.onEditAddress(true) }
                    .padding(horizontal = 14.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                if (state.editing) {
                    val focus = remember { FocusRequester() }
                    LaunchedEffect(Unit) { focus.requestFocus() }
                    BasicTextField(
                        value = state.address,
                        onValueChange = { listener.onAddressChanged(it) },
                        singleLine = true,
                        textStyle = MaterialTheme.typography.bodyMedium.copy(color = MaterialTheme.colorScheme.onSurface),
                        cursorBrush = SolidColor(MaterialTheme.colorScheme.primary),
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Go),
                        keyboardActions = KeyboardActions(onGo = { listener.onGo(state.address) }),
                        modifier = Modifier.fillMaxWidth().focusRequester(focus),
                    )
                } else {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        SiteIcon(state.url, 16.dp)
                        val title = state.title.takeIf { it.isNotEmpty() && it != state.url }
                        Text(
                            buildString {
                                if (title != null) append(title).append(" · ")
                                append(state.host.removePrefix("www."))
                            },
                            style = MaterialTheme.typography.bodyMedium,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
            }
            OpenInSpace(state, listener)
        }
    }
}

/**
 * "Open in <space>", and ⌄ with the window's profile's spaces when it has more than one, and
 * Always Open <domain> in › (link-routing.md) when the page has a domain.
 */
@Composable
private fun OpenInSpace(state: PeekUi.State, listener: PeekUi.Listener) {
    var menu by remember { mutableStateOf(false) }
    // The menu's second level: the spaces for Always Open <domain> in.
    var always by remember { mutableStateOf(false) }
    val showMenu = state.spaces.size > 1 || state.ruleDomain.isNotEmpty()
    Row(
        Modifier.height(40.dp).clip(CircleShape).background(MaterialTheme.colorScheme.secondaryContainer),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            if (state.spaceName.isEmpty()) "Open" else "Open in ${state.spaceName}",
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSecondaryContainer,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier =
                Modifier.widthIn(max = 160.dp)
                    .clickable { listener.onOpen("") }
                    .padding(start = 14.dp, end = if (showMenu) 6.dp else 14.dp)
                    .padding(vertical = 10.dp),
        )
        if (showMenu) {
            Box {
                Box(
                    Modifier.size(40.dp).clickable {
                        always = false
                        menu = true
                    },
                    contentAlignment = Alignment.Center,
                ) {
                    Sym(
                        R.drawable.compass_sym_chevron_right,
                        14f,
                        MaterialTheme.colorScheme.onSecondaryContainer,
                        Modifier.rotate(90f),
                    )
                }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    if (always) {
                        DropdownMenuItem(
                            text = {
                                Text(
                                    "Always Open ${state.ruleDomain} in",
                                    style = MaterialTheme.typography.labelLarge,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            },
                            leadingIcon = {
                                Sym(R.drawable.compass_sym_chevron_right, 14f, modifier = Modifier.rotate(180f))
                            },
                            onClick = { always = false },
                        )
                        for (space in state.spaces) {
                            DropdownMenuItem(
                                text = { Text(space.name.ifEmpty { "Space" }) },
                                leadingIcon = { SpaceBadge(space) },
                                trailingIcon = {
                                    if (space.id == state.ruleSpaceId) Sym(R.drawable.compass_sym_check, 14f)
                                },
                                onClick = {
                                    menu = false
                                    listener.onAlwaysOpenIn(space.id)
                                },
                            )
                        }
                    } else {
                        if (state.spaces.size > 1) {
                            for (space in state.spaces) {
                                DropdownMenuItem(
                                    text = { Text(space.name.ifEmpty { "Space" }) },
                                    leadingIcon = { SpaceBadge(space) },
                                    trailingIcon = {
                                        if (space.id == state.spaceId) Sym(R.drawable.compass_sym_check, 14f)
                                    },
                                    onClick = {
                                        menu = false
                                        listener.onOpen(space.id)
                                    },
                                )
                            }
                        }
                        if (state.ruleDomain.isNotEmpty()) {
                            if (state.spaces.size > 1) HorizontalDivider()
                            DropdownMenuItem(
                                text = { Text("Always Open ${state.ruleDomain} in") },
                                trailingIcon = { Sym(R.drawable.compass_sym_chevron_right, 14f) },
                                onClick = { always = true },
                            )
                        }
                    }
                }
            }
        }
    }
}

/** A space's emoji, or its initial on its color. */
@Composable
private fun SpaceBadge(space: PeekUi.SpaceRow) {
    val color = CompassStyle.parse(space.tint) ?: MaterialTheme.colorScheme.surfaceContainerHighest
    Box(Modifier.size(24.dp).clip(CircleShape).background(color), contentAlignment = Alignment.Center) {
        Text(
            space.emoji.ifEmpty { space.name.take(1).uppercase() },
            fontSize = 12.sp,
            fontWeight = FontWeight.SemiBold,
            color = if (space.emoji.isEmpty()) Color.White else Color.Unspecified,
        )
    }
}

/** A sheet's drag handle. */
@Composable
private fun Grabber() {
    Box(Modifier.fillMaxWidth().padding(top = 8.dp, bottom = 6.dp), contentAlignment = Alignment.Center) {
        Box(
            Modifier.width(32.dp)
                .height(4.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f))
        )
    }
}

@Composable
private fun IconCircle(id: Int, enabled: Boolean = true, onClick: () -> Unit) {
    Box(
        Modifier.size(40.dp).clip(CircleShape).clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Sym(id, 17f, modifier = Modifier.alpha(if (enabled) 1f else 0.35f))
    }
}
