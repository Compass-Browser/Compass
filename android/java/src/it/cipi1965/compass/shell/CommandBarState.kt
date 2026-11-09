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

    var open by mutableStateOf(false)
    var text by mutableStateOf("")
    var suggestions by mutableStateOf<List<Suggestion>>(emptyList())
    var entries by mutableStateOf<List<Entry>>(emptyList())
    var incognito by mutableStateOf(false)
    /** Editing the page's address ("Go to"), not a new tab. */
    var editing by mutableStateOf(false)
    var spaceName by mutableStateOf("")
    /** The space's tint ("#rrggbb", or empty). */
    var tint by mutableStateOf("")
    /** The typed text reads as an address, not a search. */
    var typedIsAddress by mutableStateOf(false)
}
