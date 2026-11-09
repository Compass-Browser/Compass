// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.graphics.Bitmap
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** What the tab picker shows: the window's tabs as cards (TabPicker.java). */
class TabPickerState {
    class Card(
        @JvmField val id: Int,
        @JvmField val title: String,
        /** The page's host, under the title. */
        @JvmField val host: String,
    )

    var open by mutableStateOf(false)
    /** Least recently shown first; the tab on screen is centered when the picker opens. */
    var cards by mutableStateOf<List<Card>>(emptyList())
    var currentId by mutableStateOf(-1)
    var incognito by mutableStateOf(false)
    /** Whether the window has incognito tabs to switch to (or is showing them). */
    var hasIncognito by mutableStateOf(false)
    /** The space's tint behind the cards ("#rrggbb", or empty). */
    var tint by mutableStateOf("")
    /** Card ID -> its page as it was last shown. */
    val thumbnails = mutableStateMapOf<Int, Bitmap>()
    /** The content area's width / height, for the cards' shape. */
    var aspect by mutableStateOf(0.5f)
}
