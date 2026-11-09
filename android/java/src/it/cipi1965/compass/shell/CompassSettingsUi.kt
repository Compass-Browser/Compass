// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.compose.BackHandler
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import it.cipi1965.compass.R
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner

/**
 * Compass's settings, as the iOS app's (SettingsView.swift): grouped rows with colored icons.
 * Compass's own screens (sync, Claude, extensions) open from here, and so do the Chrome settings
 * pages Compass keeps (search engine, passwords, site settings, …), each at its page.
 */
object CompassSettingsUi {
    /** The rows, by what they open. */
    enum class Row {
        SYNC,
        SEARCH_ENGINE,
        PASSWORDS,
        AUTOFILL_APPS,
        CLAUDE,
        EXTENSIONS,
        CLEAR_DATA,
        DOWNLOADS,
        SITE_SETTINGS,
        PRIVACY,
        ACCESSIBILITY,
        LANGUAGES,
        MORE,
        PRIVACY_POLICY,
    }

    interface Listener {
        fun onRow(row: Row)

        fun onDismiss()
    }

    class State {
        var open by mutableStateOf(false)
        var version by mutableStateOf("")
        /** Sync's line under "Sync": on, off, last synced. */
        var syncSummary by mutableStateOf("")
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: State,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) {
                if (state.open) Settings(state, listener)
            }
        }
        return view
    }
}

@Composable
private fun Settings(state: CompassSettingsUi.State, listener: CompassSettingsUi.Listener) {
    BackHandler { listener.onDismiss() }
    fun open(row: CompassSettingsUi.Row) = listener.onRow(row)
    Box(
        Modifier.fillMaxSize()
            .background(MaterialTheme.colorScheme.surfaceContainerLow)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {}
            .windowInsetsPadding(WindowInsets.safeDrawing)
    ) {
        Column(Modifier.fillMaxSize()) {
            // The title bar: "Settings", and Done.
            Box(Modifier.fillMaxWidth().height(52.dp).padding(horizontal = 16.dp)) {
                Text(
                    "Settings",
                    fontSize = 17.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.align(Alignment.Center),
                )
                Text(
                    "Done",
                    fontSize = 17.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.primary,
                    modifier =
                        Modifier.align(Alignment.CenterEnd).clickable { listener.onDismiss() }.padding(vertical = 8.dp),
                )
            }
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 8.dp)
            ) {
                CardSection("Sync") {
                    CardRow("Sync", state.syncSummary, first = true, trailing = R.drawable.compass_sym_chevron_right, onClick = {
                        open(CompassSettingsUi.Row.SYNC)
                    }) {
                        MenuIcon(R.drawable.compass_sym_sync, CompassStyle.blue)
                    }
                }
                CardSection("Browsing") {
                    SettingsRow("Search Engine", R.drawable.compass_sym_search, CompassStyle.blue, first = true) {
                        open(CompassSettingsUi.Row.SEARCH_ENGINE)
                    }
                    SettingsRow("Passwords", R.drawable.compass_sym_key_fill, CompassStyle.gray) {
                        open(CompassSettingsUi.Row.PASSWORDS)
                    }
                    SettingsRow(
                        "Passwords in Other Apps",
                        R.drawable.compass_sym_key_fill,
                        CompassStyle.gray,
                        subtitle = "Fill your Compass passwords in apps",
                    ) {
                        open(CompassSettingsUi.Row.AUTOFILL_APPS)
                    }
                    SettingsRow("Claude", R.drawable.compass_sym_auto_awesome, CompassStyle.orange) {
                        open(CompassSettingsUi.Row.CLAUDE)
                    }
                    SettingsRow("Extensions", R.drawable.compass_sym_extension_fill, CompassStyle.green) {
                        open(CompassSettingsUi.Row.EXTENSIONS)
                    }
                }
                CardSection("Tabs and data") {
                    SettingsRow("Clear Browsing Data", R.drawable.compass_sym_delete, CompassStyle.red, first = true) {
                        open(CompassSettingsUi.Row.CLEAR_DATA)
                    }
                    SettingsRow("Downloads", R.drawable.compass_sym_download, CompassStyle.blue) {
                        open(CompassSettingsUi.Row.DOWNLOADS)
                    }
                }
                CardSection("Privacy") {
                    SettingsRow("Site Settings", R.drawable.compass_sym_public, Color.Black, first = true) {
                        open(CompassSettingsUi.Row.SITE_SETTINGS)
                    }
                    SettingsRow("Privacy and Security", R.drawable.compass_sym_shield, CompassStyle.indigo) {
                        open(CompassSettingsUi.Row.PRIVACY)
                    }
                    SettingsRow("Privacy Policy", R.drawable.compass_sym_lock, CompassStyle.indigo) {
                        open(CompassSettingsUi.Row.PRIVACY_POLICY)
                    }
                }
                CardSection("General") {
                    SettingsRow("Accessibility", R.drawable.compass_sym_tune, CompassStyle.blue, first = true) {
                        open(CompassSettingsUi.Row.ACCESSIBILITY)
                    }
                    SettingsRow("Languages", R.drawable.compass_sym_language, CompassStyle.teal) {
                        open(CompassSettingsUi.Row.LANGUAGES)
                    }
                    SettingsRow("More Settings", R.drawable.compass_sym_settings_fill, CompassStyle.gray) {
                        open(CompassSettingsUi.Row.MORE)
                    }
                }
                Text(
                    "Compass ${state.version}",
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.fillMaxWidth().padding(vertical = 24.dp),
                )
            }
        }
    }
}

@Composable
private fun SettingsRow(
    title: String,
    icon: Int,
    color: Color,
    subtitle: String = "",
    first: Boolean = false,
    onClick: () -> Unit,
) {
    CardRow(title, subtitle, first = first, trailing = R.drawable.compass_sym_chevron_right, onClick = onClick) {
        MenuIcon(icon, color)
    }
}
