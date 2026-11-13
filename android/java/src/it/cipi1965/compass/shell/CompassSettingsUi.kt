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
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.material3.Switch
import androidx.compose.foundation.layout.ColumnScope
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
        /** Chrome's Privacy and Security page, for what Compass's doesn't show. */
        MORE_PRIVACY,
        ACCESSIBILITY,
        LANGUAGES,
        MORE,
        PRIVACY_POLICY,
    }

    /** The page on screen: the list, or one of Compass's own settings pages. */
    enum class Page {
        MAIN,
        SEARCH_ENGINE,
        PRIVACY,
    }

    /** Safe Browsing's levels (Chrome's SafeBrowsingState). */
    object SafeBrowsing {
        const val OFF = 0
        const val STANDARD = 1
        const val ENHANCED = 2
    }

    /** The switches on the Privacy page. */
    enum class PrivacySwitch {
        BLOCK_THIRD_PARTY_COOKIES,
        HTTPS_ONLY,
        PRELOAD_PAGES,
        DO_NOT_TRACK,
    }

    /** A search engine to choose (Chrome's TemplateUrlService). */
    class Engine(@JvmField val keyword: String, @JvmField val name: String, @JvmField val host: String)

    interface Listener {
        fun onRow(row: Row)

        /** Makes the engine with |keyword| the default search engine. */
        fun onSetSearchEngine(keyword: String)

        /** Privacy: Safe Browsing's level (SafeBrowsing). */
        fun onSetSafeBrowsing(level: Int)

        /** Privacy: one of its switches. */
        fun onSetPrivacySwitch(which: PrivacySwitch, on: Boolean)

        /** Back from a page to the list. */
        fun onBack()

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
        var page by mutableStateOf(Page.MAIN)
        var searchEngines by mutableStateOf<List<Engine>>(emptyList())
        /** The default search engine's keyword. */
        var searchEngine by mutableStateOf("")
        /** Set by policy: it can't be changed here. */
        var searchEngineManaged by mutableStateOf(false)
        /** Privacy: Safe Browsing's level (SafeBrowsing), and whether policy sets it. */
        var safeBrowsing by mutableStateOf(SafeBrowsing.STANDARD)
        var safeBrowsingManaged by mutableStateOf(false)
        /** Privacy: the switches that are on, and those policy sets. */
        var privacyOn by mutableStateOf<Set<PrivacySwitch>>(emptySet())
        var privacyManaged by mutableStateOf<Set<PrivacySwitch>>(emptySet())
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
                if (state.open) {
                    when (state.page) {
                        Page.SEARCH_ENGINE -> SearchEnginePage(state, listener)
                        Page.PRIVACY -> PrivacyPage(state, listener)
                        Page.MAIN -> Settings(state, listener)
                    }
                }
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
                    SettingsRow(
                        "Search Engine",
                        R.drawable.compass_sym_search,
                        subtitle = state.searchEngines.firstOrNull { it.keyword == state.searchEngine }?.name ?: "",
                        first = true,
                    ) {
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

/**
 * Settings → Search Engine, Compass's own page (docs/design/android.md, P8): the engines Chrome
 * offers (its prepopulated ones, and the default if it's another), the default checked.
 */
@Composable
private fun SearchEnginePage(state: CompassSettingsUi.State, listener: CompassSettingsUi.Listener) {
    SubPage("Search Engine", listener) {
        if (state.searchEngineManaged) {
            Text(
                "Your organization chose the search engine.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 16.dp, top = 8.dp),
            )
        }
        CardSection("Search with") {
            for (engine in state.searchEngines) {
                RadioRow(
                    engine.name,
                    engine.host,
                    selected = engine.keyword == state.searchEngine,
                    enabled = !state.searchEngineManaged,
                ) {
                    listener.onSetSearchEngine(engine.keyword)
                }
            }
        }
    }
}

/**
 * Settings → Privacy and Security, Compass's own page (docs/design/android.md, P8): Safe
 * Browsing's level and the switches people look for, as on the desktop's privacy page; the rest
 * on Chrome's page (More Privacy Settings).
 */
@Composable
private fun PrivacyPage(state: CompassSettingsUi.State, listener: CompassSettingsUi.Listener) {
    SubPage("Privacy and Security", listener) {
        CardSection("Safe Browsing") {
            val levels = listOf(
                Triple(CompassSettingsUi.SafeBrowsing.ENHANCED, "Enhanced Protection",
                    "Checks sites and downloads in real time with Google"),
                Triple(CompassSettingsUi.SafeBrowsing.STANDARD, "Standard Protection",
                    "Warns about sites and downloads known to be dangerous"),
                Triple(CompassSettingsUi.SafeBrowsing.OFF, "No Protection", "Not recommended"),
            )
            for ((level, title, subtitle) in levels) {
                RadioRow(
                    title,
                    subtitle,
                    selected = state.safeBrowsing == level,
                    enabled = !state.safeBrowsingManaged,
                ) {
                    listener.onSetSafeBrowsing(level)
                }
            }
        }
        CardSection("Browsing") {
            val switches = listOf(
                Triple(CompassSettingsUi.PrivacySwitch.BLOCK_THIRD_PARTY_COOKIES, "Block Third-Party Cookies",
                    "Sites can't use cookies to follow you across sites"),
                Triple(CompassSettingsUi.PrivacySwitch.HTTPS_ONLY, "Always Use Secure Connections",
                    "Warns before loading a site that doesn't support HTTPS"),
                Triple(CompassSettingsUi.PrivacySwitch.PRELOAD_PAGES, "Preload Pages",
                    "Loads pages you're likely to visit, for faster browsing"),
                Triple(CompassSettingsUi.PrivacySwitch.DO_NOT_TRACK, "Send \"Do Not Track\"",
                    "Asks sites not to track you; many ignore it"),
            )
            for ((which, title, subtitle) in switches) {
                SwitchRow(
                    title,
                    subtitle,
                    checked = which in state.privacyOn,
                    enabled = which !in state.privacyManaged,
                ) {
                    listener.onSetPrivacySwitch(which, it)
                }
            }
        }
        CardSection("More") {
            SettingsRow("Clear Browsing Data", R.drawable.compass_sym_delete, first = true) {
                listener.onRow(CompassSettingsUi.Row.CLEAR_DATA)
            }
            SettingsRow("More Privacy Settings", R.drawable.compass_sym_shield) {
                listener.onRow(CompassSettingsUi.Row.MORE_PRIVACY)
            }
        }
        if (state.safeBrowsingManaged || state.privacyManaged.isNotEmpty()) {
            Text(
                "Your organization manages some of these settings.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 16.dp, top = 12.dp, bottom = 24.dp),
            )
        }
    }
}

/** One of Compass's settings pages: the top app bar (back, title) over a scrolling column. */
@Composable
private fun SubPage(
    title: String,
    listener: CompassSettingsUi.Listener,
    content: @Composable ColumnScope.() -> Unit,
) {
    BackHandler { listener.onBack() }
    Box(
        Modifier.fillMaxSize()
            .background(MaterialTheme.colorScheme.surface)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {}
            .windowInsetsPadding(WindowInsets.safeDrawing)
    ) {
        Column(Modifier.fillMaxSize()) {
            Row(
                Modifier.fillMaxWidth().height(64.dp).padding(horizontal = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(
                    Modifier.size(48.dp).clip(CircleShape).clickable { listener.onBack() },
                    contentAlignment = Alignment.Center,
                ) {
                    Sym(R.drawable.compass_sym_arrow_back, 20f)
                }
                Text(title, style = MaterialTheme.typography.titleLarge, modifier = Modifier.padding(start = 4.dp))
            }
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 4.dp),
                content = content,
            )
        }
    }
}

@Composable
private fun RadioRow(title: String, subtitle: String, selected: Boolean, enabled: Boolean, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth()
            .heightIn(min = 64.dp)
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        RadioButton(selected = selected, enabled = enabled, onClick = onClick)
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge, maxLines = 1)
            if (subtitle.isNotEmpty()) {
                Text(subtitle, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
private fun SwitchRow(
    title: String,
    subtitle: String,
    checked: Boolean,
    enabled: Boolean,
    onChange: (Boolean) -> Unit,
) {
    Row(
        Modifier.fillMaxWidth()
            .heightIn(min = 72.dp)
            .clickable(enabled = enabled) { onChange(!checked) }
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(subtitle, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Switch(checked = checked, enabled = enabled, onCheckedChange = onChange)
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
