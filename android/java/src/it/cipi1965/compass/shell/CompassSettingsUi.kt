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
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.Row
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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.TextButton
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

        /** Settings → On Launch: OnLaunch.SIDEBAR or OnLaunch.LAST_TAB. */
        fun onSetOnLaunch(value: String)

        fun onDismiss()
    }

    class State {
        var open by mutableStateOf(false)
        var version by mutableStateOf("")
        /** Sync's line under "Sync": on, off, last synced. */
        var syncSummary by mutableStateOf("")
        /** What a launch shows: OnLaunch.SIDEBAR or OnLaunch.LAST_TAB. */
        var onLaunch by mutableStateOf(OnLaunch.SIDEBAR)
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
    var choosingOnLaunch by remember { mutableStateOf(false) }
    if (choosingOnLaunch) {
        OnLaunchDialog(state.onLaunch, onPick = {
            choosingOnLaunch = false
            listener.onSetOnLaunch(it)
        }) { choosingOnLaunch = false }
    }
    Box(
        Modifier.fillMaxSize()
            .background(MaterialTheme.colorScheme.surface)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {}
            .windowInsetsPadding(WindowInsets.safeDrawing)
    ) {
        Column(Modifier.fillMaxSize()) {
            // Material's top app bar: back, and the title.
            Row(
                Modifier.fillMaxWidth().height(64.dp).padding(horizontal = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(
                    Modifier.size(48.dp).clip(CircleShape).clickable { listener.onDismiss() },
                    contentAlignment = Alignment.Center,
                ) {
                    Sym(R.drawable.compass_sym_arrow_back, 20f)
                }
                Text(
                    "Settings",
                    style = MaterialTheme.typography.titleLarge,
                    modifier = Modifier.padding(start = 4.dp),
                )
            }
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 4.dp)
            ) {
                CardSection("Sync") {
                    CardRow("Sync", state.syncSummary, first = true, onClick = {
                        open(CompassSettingsUi.Row.SYNC)
                    }) {
                        MenuIcon(R.drawable.compass_sym_sync)
                    }
                }
                CardSection("Browsing") {
                    SettingsRow("Search Engine", R.drawable.compass_sym_search, first = true) {
                        open(CompassSettingsUi.Row.SEARCH_ENGINE)
                    }
                    SettingsRow("Passwords", R.drawable.compass_sym_key_fill) {
                        open(CompassSettingsUi.Row.PASSWORDS)
                    }
                    SettingsRow(
                        "Passwords in Other Apps",
                        R.drawable.compass_sym_key_fill,
                        subtitle = "Fill your Compass passwords in apps",
                    ) {
                        open(CompassSettingsUi.Row.AUTOFILL_APPS)
                    }
                    SettingsRow("Claude", R.drawable.compass_sym_auto_awesome) {
                        open(CompassSettingsUi.Row.CLAUDE)
                    }
                    SettingsRow("Extensions", R.drawable.compass_sym_extension_fill) {
                        open(CompassSettingsUi.Row.EXTENSIONS)
                    }
                }
                CardSection("Tabs and data") {
                    SettingsRow(
                        "On Launch",
                        R.drawable.compass_sym_view_sidebar,
                        subtitle = if (state.onLaunch == OnLaunch.LAST_TAB) "Last Tab" else "Sidebar",
                        first = true,
                    ) {
                        choosingOnLaunch = true
                    }
                    SettingsRow("Clear Browsing Data", R.drawable.compass_sym_delete) {
                        open(CompassSettingsUi.Row.CLEAR_DATA)
                    }
                    SettingsRow("Downloads", R.drawable.compass_sym_download) {
                        open(CompassSettingsUi.Row.DOWNLOADS)
                    }
                }
                CardSection("Privacy") {
                    SettingsRow("Site Settings", R.drawable.compass_sym_public, first = true) {
                        open(CompassSettingsUi.Row.SITE_SETTINGS)
                    }
                    SettingsRow("Privacy and Security", R.drawable.compass_sym_shield) {
                        open(CompassSettingsUi.Row.PRIVACY)
                    }
                    SettingsRow("Privacy Policy", R.drawable.compass_sym_lock) {
                        open(CompassSettingsUi.Row.PRIVACY_POLICY)
                    }
                }
                CardSection("General") {
                    SettingsRow("Accessibility", R.drawable.compass_sym_tune, first = true) {
                        open(CompassSettingsUi.Row.ACCESSIBILITY)
                    }
                    SettingsRow("Languages", R.drawable.compass_sym_language) {
                        open(CompassSettingsUi.Row.LANGUAGES)
                    }
                    SettingsRow("More Settings", R.drawable.compass_sym_settings_fill) {
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

/** On Launch's choices (AppModel.swift, StartScreen). */
@Composable
private fun OnLaunchDialog(value: String, onPick: (String) -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("On Launch") },
        text = {
            Column {
                for ((choice, label) in listOf(OnLaunch.SIDEBAR to "Sidebar", OnLaunch.LAST_TAB to "Last Tab")) {
                    Row(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).clickable { onPick(choice) }.padding(vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(selected = choice == value, onClick = { onPick(choice) })
                        Text(label, style = MaterialTheme.typography.bodyLarge)
                    }
                }
                Text(
                    "Last Tab opens on the tab you last looked at, if it's still open.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 8.dp),
                )
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun SettingsRow(
    title: String,
    icon: Int,
    subtitle: String = "",
    first: Boolean = false,
    onClick: () -> Unit,
) {
    CardRow(title, subtitle, first = first, onClick = onClick) {
        MenuIcon(icon)
    }
}
