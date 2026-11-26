// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** What the command bar shows; CommandBar fills it from Chrome's autocomplete and the sidebar. */
class CommandBarState {
    /** One of autocomplete's rows: a page (title and URL) or a search. */
    class Suggestion(
        @JvmField val title: String,
        @JvmField val subtitle: String,
        @JvmField val isSearch: Boolean,
        @JvmField val url: String,
    )

    /** A sidebar entry to switch to: where it is ("Pinned", "Today", "Favorites"). */
    class Entry(
        @JvmField val id: String,
        @JvmField val title: String,
        @JvmField val url: String,
        @JvmField val where: String,
    )

    /** A tab open on another device of the sync chain (command-bar.md, "Tabs on other devices"). */
    class DeviceTab(@JvmField val title: String, @JvmField val url: String, @JvmField val device: String)

    /** Another device's open tabs, as sync has them; |modified|: when it last changed. */
    class Device(@JvmField val name: String, @JvmField val modified: Long, @JvmField val tabs: List<DeviceTab>)

    companion object {
        /** At most this many tabs on other devices. */
        const val MAX_DEVICE_TABS = 6

        /**
         * The other devices' tabs whose title, URL or device name contains every word of |query|,
         * at most MAX_DEVICE_TABS, grouped by device, the most recently active device first. None
         * for an empty query.
         */
        @JvmStatic
        fun matchDeviceTabs(devices: List<Device>, query: String): List<DeviceTab> {
            val words = query.lowercase().split(' ').filter { it.isNotEmpty() }
            if (words.isEmpty()) return emptyList()
            val matches = ArrayList<DeviceTab>()
            for (device in devices.sortedByDescending { it.modified }) {
                for (tab in device.tabs) {
                    val haystack = "${tab.title}\n${tab.url}\n${device.name}".lowercase()
                    if (words.all { haystack.contains(it) }) {
                        matches.add(tab)
                        if (matches.size == MAX_DEVICE_TABS) return matches
                    }
                }
            }
            return matches
        }
    }

    var open by mutableStateOf(false)
    var text by mutableStateOf("")
    var suggestions by mutableStateOf<List<Suggestion>>(emptyList())
    var entries by mutableStateOf<List<Entry>>(emptyList())
    var deviceTabs by mutableStateOf<List<DeviceTab>>(emptyList())
    var incognito by mutableStateOf(false)
    /** Editing the page's address ("Go to"), not a new tab. */
    var editing by mutableStateOf(false)
    var spaceName by mutableStateOf("")
    /** The space's tint ("#rrggbb", or empty). */
    var tint by mutableStateOf("")
    /** The space's whole theme, behind the sheet. */
    var theme by mutableStateOf(SpaceTheme.GREY)
    /** The typed text reads as an address, not a search. */
    var typedIsAddress by mutableStateOf(false)

    // Ask in the command bar (docs/design/ai-sidebar.md, "Ask in the command bar").
    /** The typed text reads as a question and Ask is on: "Ask Claude" comes first. */
    var askOffered by mutableStateOf(false)
    /** That row's title: "Ask Claude", "Ask GPT"… (AiNames.askTitle()). */
    var askTitle by mutableStateOf("")
    /** The page "About this page" names (its title), or empty when there's no web page. */
    var aboutPageTitle by mutableStateOf("")
    /** "About this page": the page's title and host go with the question. On by default. */
    var aboutPage by mutableStateOf(true)
    /** The question asked, while its answer shows in place of the results; empty: none. */
    var asked by mutableStateOf("")
    /** The answer so far, whether it's all there, and why it isn't (empty: no error). */
    var answer by mutableStateOf("")
    var answerDone by mutableStateOf(false)
    var answerError by mutableStateOf("")
}
