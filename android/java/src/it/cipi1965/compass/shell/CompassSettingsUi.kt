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
import androidx.compose.material3.OutlinedTextField
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
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import it.cipi1965.compass.R
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.rememberTextFieldState

/**
 * Compass's settings, as the iOS app's (SettingsView.swift): grouped rows with colored icons.
 * Compass's own screens (sync, Claude, extensions) open from here, and so do the Chrome settings
 * pages Compass keeps (search engine, passwords, site settings, …), each at its page.
 */
object CompassSettingsUi {
    /** The rows, by what they open. */
    enum class Row {
        SYNC,
        /** Android's choice of browser app: Compass, or another. */
        DEFAULT_BROWSER,
        SEARCH_ENGINE,
        PASSWORDS,
        AUTOFILL_APPS,
        CLAUDE,
        EXTENSIONS,
        CLEAR_DATA,
        DOWNLOADS,
        SITE_SETTINGS,
        PRIVACY,
        /** The ad blocker: its switch and filter lists (docs/design/ad-blocker.md). */
        AD_BLOCKER,
        /** Chrome's Privacy and Security page, for what Compass's doesn't show. */
        MORE_PRIVACY,
        /** Chrome's site settings: every site, and each permission's exceptions. */
        ALL_SITES,
        /** Sites in Spaces: links from other apps routed by domain (link-routing.md). */
        SITES_IN_SPACES,
        ACCESSIBILITY,
        LANGUAGES,
        MORE,
        PRIVACY_POLICY,
        /** The licenses of the files Compass adds to Chromium (third_party/). */
        LICENSES,
    }

    /** The page on screen: the list, or one of Compass's own settings pages. */
    enum class Page {
        MAIN,
        SEARCH_ENGINE,
        PRIVACY,
        AD_BLOCKER,
        SITE_SETTINGS,
        SITES_IN_SPACES,
        LICENSES,
    }

    /** A filter list on the Ad Blocker page (AdBlockBridge.listsJson()). */
    class FilterListRow(
        @JvmField val id: String,
        @JvmField val title: String,
        /** EasyList, EasyPrivacy and the cookie list: turned off, not removed. */
        @JvmField val builtin: Boolean,
        @JvmField val enabled: Boolean,
        /** "12,345 rules · updated 2 hours ago", "Not downloaded yet", "Updating…". */
        @JvmField val detail: String,
        /** The last download's error, empty if it worked. */
        @JvmField val error: String,
    )

    /** A third-party file's license, on the Third-Party Licenses page. */
    class License(
        @JvmField val title: String,
        /** What Compass uses it for, and its source. */
        @JvmField val about: String,
        @JvmField val text: String,
    )

    /** A space a rule can name: this window's profile's. */
    class SpaceChoice(
        @JvmField val id: String,
        @JvmField val name: String,
        @JvmField val emoji: String,
        @JvmField val tint: String,
    )

    /** A link rule (Sites in Spaces): its domain and space, or null when the space is gone. */
    class LinkRuleRow(@JvmField val domain: String, @JvmField val space: SpaceChoice?)

    /** A permission's default on the Site Settings page: on asks first (or allows), off blocks. */
    class SitePermission(
        @JvmField val type: Int,
        @JvmField val title: String,
        /** What on means: "Ask first" or "Allowed". */
        @JvmField val onLabel: String,
        @JvmField val on: Boolean,
        @JvmField val managed: Boolean,
    )

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
        CLEAN_COPIED_LINKS,
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

        /** Site Settings: a permission's default (SitePermission.type, Chrome's ContentSettingsType). */
        fun onSetSitePermission(type: Int, on: Boolean)

        /** Site Settings: picture-in-picture also when Compass is left for another app. */
        fun onSetPipOnLeaving(on: Boolean)

        /** Ad Blocker: its switch, for every site and profile. */
        fun onSetAdBlock(on: Boolean)

        /** Ad Blocker: a filter list on or off. */
        fun onSetFilterList(id: String, on: Boolean)

        /** Ad Blocker: adds the list at |url|; why not, empty when added. */
        fun onAddFilterList(url: String): String

        fun onRemoveFilterList(id: String)

        /** Ad Blocker: asks every enabled list's server for a newer copy. */
        fun onUpdateFilterLists()

        /** Back from a page to the list. */
        fun onBack()

        /** Settings → On Launch: OnLaunch.SIDEBAR or OnLaunch.LAST_TAB. */
        fun onSetOnLaunch(value: String)

        /** Settings → Open in a Little Window (LittleWindow). */
        fun onSetLittleWindow(on: Boolean)

        /** Sites in Spaces: |input| (a domain or a URL) as a rule's domain, or empty. */
        fun normalizeLinkDomain(input: String): String

        /** Sites in Spaces: |domain|'s rule names |spaceId| (made or changed). */
        fun onSetLinkRule(domain: String, spaceId: String)

        fun onRemoveLinkRule(domain: String)

        fun onDismiss()
    }

    /** A profile of the sync chain (Settings › Profiles): its name and how many spaces it has. */
    class ProfileRow(
        @JvmField val name: String,
        @JvmField val spaces: Int,
        /** The profile this window shows. */
        @JvmField val current: Boolean,
    )

    class State {
        var open by mutableStateOf(false)
        /** The chain's profiles, when there's more than one (docs/design/android.md, "Profiles"). */
        var profiles by mutableStateOf<List<ProfileRow>>(emptyList())
        var version by mutableStateOf("")
        /** Compass is Android's browser app (RoleManager.ROLE_BROWSER). */
        var isDefaultBrowser by mutableStateOf(false)
        /** Sync's line under "Sync": on, off, last synced. */
        var syncSummary by mutableStateOf("")
        /** What a launch shows: OnLaunch.SIDEBAR or OnLaunch.LAST_TAB. */
        var onLaunch by mutableStateOf(OnLaunch.SIDEBAR)
        /** Links from other apps open in a little window (LittleWindow). */
        var littleWindow by mutableStateOf(true)
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
        var sitePermissions by mutableStateOf<List<SitePermission>>(emptyList())
        /** Sites in Spaces: the rules, by domain, and the spaces they can name. */
        var linkRules by mutableStateOf<List<LinkRuleRow>>(emptyList())
        var ruleSpaces by mutableStateOf<List<SpaceChoice>>(emptyList())
        /** Automatic picture-in-picture also when Compass is left for another app. */
        var pipOnLeaving by mutableStateOf(true)
        var licenses by mutableStateOf<List<License>>(emptyList())
        /** The ad blocker's switch, its lists, and whether one is downloading. */
        var adBlock by mutableStateOf(true)
        var filterLists by mutableStateOf<List<FilterListRow>>(emptyList())
        var filterListsUpdating by mutableStateOf(false)
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
                        Page.AD_BLOCKER -> AdBlockerPage(state, listener)
                        Page.SITE_SETTINGS -> SiteSettingsPage(state, listener)
                        Page.SITES_IN_SPACES -> SitesInSpacesPage(state, listener)
                        Page.LICENSES -> LicensesPage(state, listener)
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
                    stringResource(R.string.compass_settings),
                    style = MaterialTheme.typography.titleLarge,
                    modifier = Modifier.padding(start = 4.dp),
                )
            }
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 4.dp)
            ) {
                CardSection(stringResource(R.string.compass_settings_sync)) {
                    CardRow(stringResource(R.string.compass_settings_sync), state.syncSummary, first = true, onClick = {
                        open(CompassSettingsUi.Row.SYNC)
                    }) {
                        MenuIcon(R.drawable.compass_sym_sync)
                    }
                }
                // Each profile of the chain and its spaces, as iOS's Settings › Profiles.
                if (state.profiles.size > 1) {
                    CardSection(stringResource(R.string.compass_settings_profiles)) {
                        val unnamed = stringResource(R.string.compass_settings_profile)
                        state.profiles.forEachIndexed { i, profile ->
                            val spaces = pluralStringResource(
                                R.plurals.compass_settings_profile_spaces, profile.spaces, profile.spaces)
                            CardRow(
                                profile.name.ifEmpty { unnamed },
                                if (profile.current) stringResource(R.string.compass_settings_profile_this_window, spaces)
                                else spaces,
                                first = i == 0,
                                onClick = {},
                            ) {
                                MenuIcon(R.drawable.compass_sym_account_circle)
                            }
                        }
                    }
                }
                CardSection(stringResource(R.string.compass_settings_browsing)) {
                    SettingsRow(
                        stringResource(R.string.compass_settings_default_browser),
                        R.drawable.compass_sym_open_in_browser,
                        subtitle =
                            if (state.isDefaultBrowser) stringResource(R.string.compass_settings_default_browser_on)
                            else stringResource(R.string.compass_settings_default_browser_off),
                        first = true,
                    ) {
                        open(CompassSettingsUi.Row.DEFAULT_BROWSER)
                    }
                    SwitchRow(
                        stringResource(R.string.compass_settings_little_window),
                        stringResource(R.string.compass_settings_little_window_summary),
                        checked = state.littleWindow,
                        enabled = true,
                    ) {
                        listener.onSetLittleWindow(it)
                    }
                    SettingsRow(
                        stringResource(R.string.compass_settings_sites_in_spaces),
                        R.drawable.compass_sym_link,
                        subtitle = stringResource(R.string.compass_settings_sites_in_spaces_summary),
                    ) {
                        open(CompassSettingsUi.Row.SITES_IN_SPACES)
                    }
                    SettingsRow(
                        stringResource(R.string.compass_settings_search_engine),
                        R.drawable.compass_sym_search,
                        subtitle = state.searchEngines.firstOrNull { it.keyword == state.searchEngine }?.name ?: "",
                    ) {
                        open(CompassSettingsUi.Row.SEARCH_ENGINE)
                    }
                    SettingsRow(stringResource(R.string.compass_settings_passwords), R.drawable.compass_sym_key_fill) {
                        open(CompassSettingsUi.Row.PASSWORDS)
                    }
                    SettingsRow(
                        stringResource(R.string.compass_settings_autofill_apps),
                        R.drawable.compass_sym_key_fill,
                        subtitle = stringResource(R.string.compass_settings_autofill_apps_summary),
                    ) {
                        open(CompassSettingsUi.Row.AUTOFILL_APPS)
                    }
                    SettingsRow(stringResource(R.string.compass_ai), R.drawable.compass_sym_auto_awesome) {
                        open(CompassSettingsUi.Row.CLAUDE)
                    }
                    SettingsRow(stringResource(R.string.compass_settings_extensions), R.drawable.compass_sym_extension_fill) {
                        open(CompassSettingsUi.Row.EXTENSIONS)
                    }
                }
                CardSection(stringResource(R.string.compass_settings_tabs_and_data)) {
                    SettingsRow(
                        stringResource(R.string.compass_settings_on_launch),
                        R.drawable.compass_sym_view_sidebar,
                        subtitle = stringResource(
                            if (state.onLaunch == OnLaunch.LAST_TAB) R.string.compass_settings_on_launch_last_tab
                            else R.string.compass_settings_on_launch_sidebar),
                        first = true,
                    ) {
                        choosingOnLaunch = true
                    }
                    SettingsRow(stringResource(R.string.compass_settings_clear_data), R.drawable.compass_sym_delete) {
                        open(CompassSettingsUi.Row.CLEAR_DATA)
                    }
                    SettingsRow(stringResource(R.string.compass_settings_downloads), R.drawable.compass_sym_download) {
                        open(CompassSettingsUi.Row.DOWNLOADS)
                    }
                }
                CardSection(stringResource(R.string.compass_settings_privacy)) {
                    SettingsRow(stringResource(R.string.compass_settings_site_settings), R.drawable.compass_sym_public, first = true) {
                        open(CompassSettingsUi.Row.SITE_SETTINGS)
                    }
                    SettingsRow(
                        stringResource(R.string.compass_settings_ad_blocker),
                        R.drawable.compass_sym_shield,
                        subtitle = stringResource(
                            if (state.adBlock) R.string.compass_settings_ad_blocker_on else R.string.compass_off),
                    ) {
                        open(CompassSettingsUi.Row.AD_BLOCKER)
                    }
                    SettingsRow(stringResource(R.string.compass_settings_privacy_and_security), R.drawable.compass_sym_lock) {
                        open(CompassSettingsUi.Row.PRIVACY)
                    }
                    SettingsRow(stringResource(R.string.compass_settings_privacy_policy), R.drawable.compass_sym_description) {
                        open(CompassSettingsUi.Row.PRIVACY_POLICY)
                    }
                }
                CardSection(stringResource(R.string.compass_settings_general)) {
                    SettingsRow(stringResource(R.string.compass_settings_accessibility), R.drawable.compass_sym_tune, first = true) {
                        open(CompassSettingsUi.Row.ACCESSIBILITY)
                    }
                    SettingsRow(stringResource(R.string.compass_settings_languages), R.drawable.compass_sym_language) {
                        open(CompassSettingsUi.Row.LANGUAGES)
                    }
                    SettingsRow(stringResource(R.string.compass_settings_more_settings), R.drawable.compass_sym_settings_fill) {
                        open(CompassSettingsUi.Row.MORE)
                    }
                    SettingsRow(stringResource(R.string.compass_settings_licenses), R.drawable.compass_sym_description) {
                        open(CompassSettingsUi.Row.LICENSES)
                    }
                }
                Text(
                    stringResource(R.string.compass_settings_version, state.version),
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
    SubPage(stringResource(R.string.compass_settings_search_engine), listener) {
        if (state.searchEngineManaged) {
            Text(
                stringResource(R.string.compass_settings_search_engine_managed),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 16.dp, top = 8.dp),
            )
        }
        CardSection(stringResource(R.string.compass_settings_search_with)) {
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
    SubPage(stringResource(R.string.compass_settings_privacy_and_security), listener) {
        CardSection(stringResource(R.string.compass_settings_safe_browsing)) {
            val levels = listOf(
                Triple(CompassSettingsUi.SafeBrowsing.ENHANCED, stringResource(R.string.compass_settings_safe_browsing_enhanced),
                    stringResource(R.string.compass_settings_safe_browsing_enhanced_summary)),
                Triple(CompassSettingsUi.SafeBrowsing.STANDARD, stringResource(R.string.compass_settings_safe_browsing_standard),
                    stringResource(R.string.compass_settings_safe_browsing_standard_summary)),
                Triple(CompassSettingsUi.SafeBrowsing.OFF, stringResource(R.string.compass_settings_safe_browsing_off),
                    stringResource(R.string.compass_settings_safe_browsing_off_summary)),
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
        CardSection(stringResource(R.string.compass_settings_browsing)) {
            val switches = listOf(
                Triple(CompassSettingsUi.PrivacySwitch.BLOCK_THIRD_PARTY_COOKIES, stringResource(R.string.compass_settings_third_party_cookies),
                    stringResource(R.string.compass_settings_third_party_cookies_summary)),
                Triple(CompassSettingsUi.PrivacySwitch.HTTPS_ONLY, stringResource(R.string.compass_settings_https_only),
                    stringResource(R.string.compass_settings_https_only_summary)),
                Triple(CompassSettingsUi.PrivacySwitch.PRELOAD_PAGES, stringResource(R.string.compass_settings_preload_pages),
                    stringResource(R.string.compass_settings_preload_pages_summary)),
                Triple(CompassSettingsUi.PrivacySwitch.DO_NOT_TRACK, stringResource(R.string.compass_settings_do_not_track),
                    stringResource(R.string.compass_settings_do_not_track_summary)),
                Triple(CompassSettingsUi.PrivacySwitch.CLEAN_COPIED_LINKS,
                    stringResource(R.string.compass_settings_clean_copied_links),
                    stringResource(R.string.compass_settings_clean_copied_links_summary)),
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
        CardSection(stringResource(R.string.compass_more)) {
            SettingsRow(stringResource(R.string.compass_settings_clear_data), R.drawable.compass_sym_delete, first = true) {
                listener.onRow(CompassSettingsUi.Row.CLEAR_DATA)
            }
            SettingsRow(stringResource(R.string.compass_settings_more_privacy), R.drawable.compass_sym_shield) {
                listener.onRow(CompassSettingsUi.Row.MORE_PRIVACY)
            }
        }
        if (state.safeBrowsingManaged || state.privacyManaged.isNotEmpty()) {
            Text(
                stringResource(R.string.compass_settings_managed),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 16.dp, top = 12.dp, bottom = 24.dp),
            )
        }
    }
}

/**
 * Settings → Ad Blocker (docs/design/ad-blocker.md), as the desktop's Privacy › Ad blocker: the
 * switch, the filter lists (each on or off; the user's own can be removed), adding a list by its
 * address, and Update Now. A site's own switch is in the Site sheet.
 */
@Composable
private fun AdBlockerPage(state: CompassSettingsUi.State, listener: CompassSettingsUi.Listener) {
    var adding by remember { mutableStateOf(false) }
    SubPage(stringResource(R.string.compass_settings_ad_blocker), listener) {
        CardSection("") {
            SwitchRow(
                stringResource(R.string.compass_settings_block_ads),
                stringResource(R.string.compass_settings_block_ads_summary),
                checked = state.adBlock,
                enabled = true,
            ) {
                listener.onSetAdBlock(it)
            }
        }
        CardSection(stringResource(R.string.compass_settings_filter_lists)) {
            for (list in state.filterLists) {
                Row(
                    Modifier.fillMaxWidth()
                        .heightIn(min = 72.dp)
                        .clickable { listener.onSetFilterList(list.id, !list.enabled) }
                        .padding(horizontal = 16.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Column(Modifier.weight(1f)) {
                        Text(list.title, style = MaterialTheme.typography.bodyLarge)
                        Text(
                            list.detail,
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        if (list.error.isNotEmpty()) {
                            Text(
                                list.error,
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.error,
                            )
                        }
                    }
                    if (!list.builtin) {
                        TextButton(onClick = { listener.onRemoveFilterList(list.id) }) {
                            Text(stringResource(R.string.compass_remove), color = MaterialTheme.colorScheme.error)
                        }
                    }
                    Switch(
                        checked = list.enabled,
                        onCheckedChange = { listener.onSetFilterList(list.id, it) },
                    )
                }
            }
        }
        CardSection("") {
            SettingsRow(stringResource(R.string.compass_settings_add_list), R.drawable.compass_sym_add, first = true) { adding = true }
            SettingsRow(
                stringResource(
                    if (state.filterListsUpdating) R.string.compass_settings_filter_lists_updating
                    else R.string.compass_settings_filter_lists_update_now),
                R.drawable.compass_sym_refresh,
                subtitle = stringResource(R.string.compass_settings_filter_lists_update_summary),
            ) {
                if (!state.filterListsUpdating) listener.onUpdateFilterLists()
            }
        }
        Text(
            stringResource(R.string.compass_settings_filter_lists_footer),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(start = 16.dp, top = 12.dp, bottom = 24.dp),
        )
    }
    if (adding) {
        AddFilterListDialog(listener) { adding = false }
    }
}

/** Add a List: an Adblock Plus or uBlock Origin list's https address. */
@Composable
private fun AddFilterListDialog(listener: CompassSettingsUi.Listener, onDone: () -> Unit) {
    val urlField = rememberTextFieldState("")
    var error by remember { mutableStateOf("") }
    AlertDialog(
        onDismissRequest = onDone,
        title = { Text(stringResource(R.string.compass_settings_add_list)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    state = urlField,
                    label = { Text(stringResource(R.string.compass_settings_add_list_address)) },
                    placeholder = { Text("https://") },
                    lineLimits = TextFieldLineLimits.SingleLine,
                    modifier = Modifier.fillMaxWidth(),
                )
                val hint = stringResource(R.string.compass_settings_add_list_hint)
                Text(
                    error.ifEmpty { hint },
                    style = MaterialTheme.typography.bodySmall,
                    color =
                        if (error.isEmpty()) MaterialTheme.colorScheme.onSurfaceVariant
                        else MaterialTheme.colorScheme.error,
                )
            }
        },
        confirmButton = {
            TextButton(
                enabled = urlField.text.isNotBlank(),
                onClick = {
                    error = listener.onAddFilterList(urlField.text.toString())
                    if (error.isEmpty()) onDone()
                },
            ) { Text(stringResource(R.string.compass_add)) }
        },
        dismissButton = { TextButton(onClick = onDone) { Text(stringResource(R.string.compass_cancel)) } },
    )
}

/**
 * Settings → Site Settings, Compass's own page (docs/design/android.md, P8): each permission's
 * default for every site; a site's own exceptions on Chrome's pages (All Sites).
 */
@Composable
private fun SiteSettingsPage(state: CompassSettingsUi.State, listener: CompassSettingsUi.Listener) {
    SubPage(stringResource(R.string.compass_settings_site_settings), listener) {
        CardSection(stringResource(R.string.compass_settings_by_default)) {
            for (permission in state.sitePermissions) {
                SwitchRow(
                    permission.title,
                    if (permission.on) permission.onLabel else stringResource(R.string.compass_settings_permission_blocked),
                    checked = permission.on,
                    enabled = !permission.managed,
                ) {
                    listener.onSetSitePermission(permission.type, it)
                }
                // "When You Leave Compass" (state.pipOnLeaving) comes with leaving the app, which
                // Android doesn't let start yet (docs/design/android.md, "Auto picture-in-picture").
            }
        }
        CardSection(stringResource(R.string.compass_settings_sites)) {
            SettingsRow(
                stringResource(R.string.compass_settings_all_sites),
                R.drawable.compass_sym_public,
                subtitle = stringResource(R.string.compass_settings_all_sites_summary),
                first = true,
            ) {
                listener.onRow(CompassSettingsUi.Row.ALL_SITES)
            }
        }
        if (state.sitePermissions.any { it.managed }) {
            Text(
                stringResource(R.string.compass_settings_managed),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 16.dp, top = 12.dp, bottom = 24.dp),
            )
        }
    }
}

/**
 * Sites in Spaces (link-routing.md; ios.md, "Links routed to spaces"): links from other apps to a
 * site open as a Today tab of its space. The rules by domain, each with its space (or "Space
 * deleted"); a tap changes or deletes one, Add Site makes one.
 */
@Composable
private fun SitesInSpacesPage(state: CompassSettingsUi.State, listener: CompassSettingsUi.Listener) {
    // The rule being edited: its domain, or "" for a new one; null: no dialog.
    var editing by remember { mutableStateOf<String?>(null) }
    SubPage(stringResource(R.string.compass_settings_sites_in_spaces), listener) {
        CardSection(stringResource(R.string.compass_settings_sites_in_spaces_header)) {
            if (state.linkRules.isEmpty()) {
                Text(
                    stringResource(R.string.compass_settings_sites_in_spaces_empty),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(16.dp),
                )
            }
            val unnamed = stringResource(R.string.compass_settings_space)
            val deleted = stringResource(R.string.compass_settings_space_deleted)
            for (rule in state.linkRules) {
                CardRow(
                    rule.domain,
                    rule.space?.name?.ifEmpty { unnamed } ?: deleted,
                    onClick = { editing = rule.domain },
                ) {
                    RuleSpaceBadge(rule.space)
                }
            }
        }
        CardSection("") {
            SettingsRow(stringResource(R.string.compass_settings_add_site), R.drawable.compass_sym_add, first = true) { editing = "" }
        }
    }
    val domain = editing
    if (domain != null) {
        LinkRuleDialog(
            domain,
            state.linkRules.firstOrNull { it.domain == domain }?.space?.id ?: "",
            state.ruleSpaces,
            listener,
            onDone = { editing = null },
        )
    }
}

@Composable
private fun RuleSpaceBadge(space: CompassSettingsUi.SpaceChoice?) {
    val color = space?.let { CompassStyle.parse(it.tint) } ?: MaterialTheme.colorScheme.surfaceContainerHighest
    Box(Modifier.size(28.dp).clip(CircleShape).background(color), contentAlignment = Alignment.Center) {
        Text(
            space?.emoji?.ifEmpty { null } ?: space?.name?.take(1)?.uppercase() ?: "?",
            fontSize = 13.sp,
            fontWeight = FontWeight.SemiBold,
            color = if (space?.emoji.isNullOrEmpty()) Color.White else Color.Unspecified,
        )
    }
}

/** A rule's form: the site (a domain or a URL, for a new rule) and its space. */
// OutlinedTextField's String overload, as the shell's other fields.
@Composable
private fun LinkRuleDialog(
    domain: String,
    spaceId: String,
    spaces: List<CompassSettingsUi.SpaceChoice>,
    listener: CompassSettingsUi.Listener,
    onDone: () -> Unit,
) {
    val adding = domain.isEmpty()
    val siteField = rememberTextFieldState(domain)
    val site = siteField.text.toString()
    var space by remember { mutableStateOf(spaceId.ifEmpty { spaces.firstOrNull()?.id ?: "" }) }
    val normalized = if (site.isBlank()) "" else listener.normalizeLinkDomain(site)
    AlertDialog(
        onDismissRequest = onDone,
        title = { Text(if (adding) stringResource(R.string.compass_settings_add_site) else domain) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (adding) {
                    OutlinedTextField(
                        state = siteField,
                        label = { Text(stringResource(R.string.compass_settings_site)) },
                        placeholder = { Text("github.com") },
                        lineLimits = TextFieldLineLimits.SingleLine,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    if (site.isNotBlank() && normalized.isEmpty()) {
                        Text(
                            stringResource(R.string.compass_settings_site_invalid),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.error,
                        )
                    } else if (normalized.isNotEmpty() && normalized != site.trim()) {
                        Text(
                            stringResource(R.string.compass_settings_site_subdomains, normalized),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                val unnamed = stringResource(R.string.compass_settings_space)
                for (choice in spaces) {
                    Row(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).clickable { space = choice.id },
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(selected = choice.id == space, onClick = { space = choice.id })
                        RuleSpaceBadge(choice)
                        Text(
                            choice.name.ifEmpty { unnamed },
                            style = MaterialTheme.typography.bodyLarge,
                            modifier = Modifier.padding(start = 12.dp),
                        )
                    }
                }
            }
        },
        confirmButton = {
            TextButton(
                enabled = space.isNotEmpty() && (if (adding) normalized.isNotEmpty() else true),
                onClick = {
                    listener.onSetLinkRule(if (adding) normalized else domain, space)
                    onDone()
                },
            ) { Text(stringResource(R.string.compass_save)) }
        },
        dismissButton = {
            Row {
                if (!adding) {
                    TextButton(onClick = {
                        listener.onRemoveLinkRule(domain)
                        onDone()
                    }) { Text(stringResource(R.string.compass_delete), color = MaterialTheme.colorScheme.error) }
                }
                TextButton(onClick = onDone) { Text(stringResource(R.string.compass_cancel)) }
            }
        },
    )
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

/**
 * Settings → Third-Party Licenses: the licenses of the files Compass adds to Chromium, shown in
 * full (CompassSettings.loadLicenses(); docs/design/shortcuts.md, "Clean links"). Chromium's own
 * are on chrome://credits.
 */
@Composable
private fun LicensesPage(state: CompassSettingsUi.State, listener: CompassSettingsUi.Listener) {
    SubPage(stringResource(R.string.compass_settings_licenses), listener) {
        for (license in state.licenses) {
            CardSection(license.title) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text(
                        license.about,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    SelectionContainer {
                        Text(
                            license.text,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 11.sp,
                            lineHeight = 15.sp,
                        )
                    }
                }
            }
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
        title = { Text(stringResource(R.string.compass_settings_on_launch)) },
        text = {
            Column {
                for ((choice, label) in listOf(
                        OnLaunch.SIDEBAR to stringResource(R.string.compass_settings_on_launch_sidebar),
                        OnLaunch.LAST_TAB to stringResource(R.string.compass_settings_on_launch_last_tab))) {
                    Row(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).clickable { onPick(choice) }.padding(vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(selected = choice == value, onClick = { onPick(choice) })
                        Text(label, style = MaterialTheme.typography.bodyLarge)
                    }
                }
                Text(
                    stringResource(R.string.compass_settings_on_launch_summary),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 8.dp),
                )
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.compass_cancel)) } },
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
