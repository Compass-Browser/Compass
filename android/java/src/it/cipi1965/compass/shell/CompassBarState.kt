// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** What the bar shows; CompassBar keeps it in step with the current tab. */
class CompassBarState {
    var host by mutableStateOf("")
    var canGoBack by mutableStateOf(false)
    var canGoForward by mutableStateOf(false)
    var incognito by mutableStateOf(false)
    var tabCount by mutableStateOf(0)
    /** The active space's emoji, or its initial. */
    var spaceLabel by mutableStateOf("")
    /** The active space's tint ("#rrggbb"), or empty. */
    var spaceTint by mutableStateOf("")
}
