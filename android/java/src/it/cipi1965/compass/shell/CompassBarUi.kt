// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.annotation.DrawableRes
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.boundsInParent
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R
import kotlin.math.abs

/**
 * Compass's bottom bar, as the iOS app's (BrowserScreen.swift): back, forward, the address, new
 * tab and the page menu, in a frosted capsule. Swiping up on it opens, by where the finger starts,
 * the tab picker (left of the address), the sidebar (on it) or the command bar (right of it);
 * swiping sideways goes to the next or previous tab. A long press on the address has the page's
 * quick actions, one on + a new incognito tab.
 */
object CompassBarUi {
    interface Actions {
        fun showSidebar()

        fun openAddress()

        fun showTabs()

        fun newTab()

        fun newIncognitoTab()

        fun goBack()

        fun goForward()

        /** The tab |offset| places over in the window's tabs (a swipe on the bar). */
        fun switchTab(offset: Int)

        fun showMenu()

        fun reload()

        fun copyUrl()

        fun share()

        fun findInPage()

        fun toggleDesktopSite()

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
                val background = MaterialTheme.colorScheme.surface.toArgb()
                SideEffect { actions.backgroundColorChanged(background) }
                Bar(state, actions)
            }
        }
        return view
    }
}

@Composable
private fun Bar(state: CompassBarState, actions: CompassBarUi.Actions) {
    var pill by remember { mutableStateOf<Rect?>(null) }
    Box(
        Modifier.fillMaxSize()
            .background(MaterialTheme.colorScheme.surface)
            .padding(start = 12.dp, end = 12.dp, top = 4.dp, bottom = 4.dp),
        contentAlignment = Alignment.Center,
    ) {
        Row(
            Modifier.fillMaxWidth()
                .height(54.dp)
                .floatingGlass(CircleShape)
                // Inside the padding: the drag's coordinates are the pill's.
                .padding(5.dp)
                .pointerInput(Unit) {
                    var start = Offset.Zero
                    var total = Offset.Zero
                    detectDragGestures(
                        onDragStart = {
                            start = it
                            total = Offset.Zero
                        },
                        onDragEnd = {
                            if (abs(total.x) > abs(total.y)) {
                                // Sideways: the next or previous tab.
                                if (abs(total.x) > 48.dp.toPx()) actions.switchTab(if (total.x < 0) 1 else -1)
                            } else if (total.y < -30.dp.toPx()) {
                                // Up: by where the finger started (thirds until the address is laid out).
                                val p = pill
                                val third = size.width / 3f
                                val left = p?.left ?: third
                                val right = p?.right ?: (2 * third)
                                when {
                                    start.x < left -> actions.showTabs()
                                    start.x <= right -> actions.showSidebar()
                                    else -> actions.newTab()
                                }
                            }
                        },
                    ) { change, amount ->
                        change.consume()
                        total += amount
                    }
                },
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            BarIcon(R.drawable.compass_sym_arrow_back, state.canGoBack) { actions.goBack() }
            BarIcon(R.drawable.compass_sym_arrow_forward, state.canGoForward) { actions.goForward() }
            AddressPill(
                state,
                actions,
                // The pill's place, for the swipe's zones.
                Modifier.weight(1f).onGloballyPositioned { pill = it.boundsInParent() },
            )
            NewTabButton(actions)
            BarIcon(R.drawable.compass_sym_more_horiz, true) { actions.showMenu() }
        }
    }
}

/** A bar button: an 18 pt glyph in a 40×44 target, faint while it can't act (BarIcon). */
@Composable
private fun BarIcon(@DrawableRes icon: Int, enabled: Boolean, onLongClick: (() -> Unit)? = null, onClick: () -> Unit) {
    Box(
        Modifier.size(width = 40.dp, height = 44.dp)
            .clip(CircleShape)
            .combinedClickable(enabled = enabled, onLongClick = onLongClick, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Sym(icon, 18f, modifier = Modifier.alpha(if (enabled) 1f else 0.3f))
    }
}

@Composable
private fun NewTabButton(actions: CompassBarUi.Actions) {
    var menu by remember { mutableStateOf(false) }
    Box {
        BarIcon(R.drawable.compass_sym_add, true, onLongClick = { menu = true }) { actions.newTab() }
        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
            MenuItem("New Tab", R.drawable.compass_sym_add) {
                menu = false
                actions.newTab()
            }
            MenuItem("New Incognito Tab", R.drawable.compass_sym_eyeglasses) {
                menu = false
                actions.newIncognitoTab()
            }
        }
    }
}

/** The address: the site's icon and host, or the search prompt (BrowserScreen.swift, addressPill). */
@Composable
private fun AddressPill(state: CompassBarState, actions: CompassBarUi.Actions, modifier: Modifier) {
    var menu by remember { mutableStateOf(false) }
    val empty = state.host.isEmpty()
    Box(modifier) {
        Row(
            Modifier.fillMaxWidth()
                .height(44.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.surfaceContainerHighest)
                .combinedClickable(
                    interactionSource = remember { MutableInteractionSource() },
                    indication = null,
                    onLongClick = if (empty) null else ({ menu = true }),
                    onClick = { actions.openAddress() },
                )
                .padding(horizontal = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(7.dp),
        ) {
            if (empty) {
                Sym(R.drawable.compass_sym_search, 14f, MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f))
                Text(
                    "Search or enter address",
                    fontSize = 16.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            } else {
                SiteIcon(state.url, 16.dp)
                Text(
                    state.host.removePrefix("www."),
                    fontSize = 16.sp,
                    fontWeight = FontWeight.Medium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
            MenuItem("Reload", R.drawable.compass_sym_refresh) {
                menu = false
                actions.reload()
            }
            MenuItem("Copy URL", R.drawable.compass_sym_content_copy) {
                menu = false
                actions.copyUrl()
            }
            MenuItem("Share", R.drawable.compass_sym_share) {
                menu = false
                actions.share()
            }
            MenuItem("Find on Page", R.drawable.compass_sym_search) {
                menu = false
                actions.findInPage()
            }
            if (state.desktopSite) {
                MenuItem("Request Mobile Site", R.drawable.compass_sym_smartphone) {
                    menu = false
                    actions.toggleDesktopSite()
                }
            } else {
                MenuItem("Request Desktop Site", R.drawable.compass_sym_desktop_windows) {
                    menu = false
                    actions.toggleDesktopSite()
                }
            }
        }
    }
}

/** A Material menu item: its icon leading. */
@Composable
internal fun MenuItem(label: String, @DrawableRes icon: Int, destructive: Boolean = false, onClick: () -> Unit) {
    val color = if (destructive) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface
    DropdownMenuItem(
        text = { Text(label, style = MaterialTheme.typography.bodyLarge, color = color) },
        leadingIcon = {
            Sym(icon, 17f, if (destructive) color else MaterialTheme.colorScheme.onSurfaceVariant)
        },
        onClick = onClick,
    )
}
