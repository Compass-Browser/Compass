// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** What the page menu shows: the page on screen and what can be done with it (PageMenu.java). */
class PageMenuState {
    var open by mutableStateOf(false)
    var url by mutableStateOf("")
    var host by mutableStateOf("")
    var canGoBack by mutableStateOf(false)
    var canGoForward by mutableStateOf(false)
    var incognito by mutableStateOf(false)
    /** The page is a pinned entry or a favorite. */
    var pinned by mutableStateOf(false)
    /** The space's tint behind the sheet ("#rrggbb", or empty). */
    var tint by mutableStateOf("")
    /** The window's extension actions. */
    var extensions by mutableStateOf<List<ExtensionsState.Action>>(emptyList())
}
