// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.graphics.Bitmap
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** What the extensions sheet shows: the window's extension actions (Extensions.java). */
class ExtensionsState {
    class Action(
        @JvmField val id: String,
        @JvmField val name: String,
        /** The action's title for the current page (its tooltip on the desktop). */
        @JvmField val title: String,
        @JvmField val icon: Bitmap?,
    )

    var open by mutableStateOf(false)
    var actions by mutableStateOf<List<Action>>(emptyList())
    /** False when this window has no extensions model (Extensions.java). */
    var available by mutableStateOf(true)
}
