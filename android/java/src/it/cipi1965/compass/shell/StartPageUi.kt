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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner

/**
 * Compass's start page, in place of Chrome's new tab page (StartPage.java): as the iOS app's empty
 * page, the space's tint.
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
    // As on iOS, a tab with no page is the space's color, nothing else: the command bar (+, or a
    // tap on the address) and the sidebar are where to go from here.
    if (state.incognito) {
        IncognitoPage(listener)
        return
    }
    val background = MaterialTheme.colorScheme.surface
    SideEffect { listener.onBackgroundColor(background.toArgb()) }
    Box(Modifier.fillMaxSize().spaceTint(state.activeSpace?.tint ?: "", background))
}

/** An incognito tab with no page, as iOS's: dark, the glasses, what incognito keeps. */
@Composable
private fun IncognitoPage(listener: StartPageUi.Listener) {
    val background = Color(0xFF171717)
    SideEffect { listener.onBackgroundColor(background.toArgb()) }
    Box(Modifier.fillMaxSize().background(background).padding(32.dp), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(14.dp)) {
            Sym(it.cipi1965.compass.R.drawable.compass_sym_eyeglasses, 44f, Color.White)
            Text(
                "Incognito tabs aren't saved or synced, leave no history, and keep their cookies " +
                    "only until you close them or quit Compass. Sites can still see your visits.",
                fontSize = 13.sp,
                color = Color.White.copy(alpha = 0.6f),
                textAlign = TextAlign.Center,
                modifier = Modifier.widthIn(max = 360.dp),
            )
        }
    }
}
