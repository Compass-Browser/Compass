// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.annotation.DrawableRes
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** What the Site sheet shows (SiteSheet.java): the page's site, its connection and settings. */
class SiteSheetState {
    enum class Connection { SECURE, NOT_SECURE, DANGEROUS, NONE }

    /** A setting of the site's: on (allowed) or off (blocked). |key| is SiteSheet's. */
    class Setting(
        @JvmField val key: Int,
        @JvmField val title: String,
        @JvmField @DrawableRes val icon: Int,
        @JvmField val on: Boolean,
        @JvmField val enabled: Boolean,
        /** What on and off read as: "Allowed" and "Blocked", or "On" and "Off". */
        @JvmField val onText: String,
        @JvmField val offText: String,
    )

    var open by mutableStateOf(false)
    var host by mutableStateOf("")
    var url by mutableStateOf("")
    var connection by mutableStateOf(Connection.NONE)
    /** The permissions the site was given or denied (none: the defaults). */
    var permissions by mutableStateOf<List<Setting>>(emptyList())
    /** Desktop site, sound, pop-ups, JavaScript. */
    var options by mutableStateOf<List<Setting>>(emptyList())
}
