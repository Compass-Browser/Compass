// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** What the command bar shows; CommandBar fills it from Chrome's autocomplete. */
class CommandBarState {
    /** One row: a page (title and URL) or a search. */
    class Suggestion(
        @JvmField val title: String,
        @JvmField val subtitle: String,
        @JvmField val isSearch: Boolean,
    )

    var open by mutableStateOf(false)
    var text by mutableStateOf("")
    var suggestions by mutableStateOf<List<Suggestion>>(emptyList())
    var incognito by mutableStateOf(false)
}
