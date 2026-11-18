// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.graphics.Bitmap
import androidx.annotation.DrawableRes
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** What the link menu shows (LinkMenu.java): its header, and its items in groups. */
class LinkMenuState {
    /** An item: |key| is LinkMenu's. |icon| 0: none; |bitmap|: an extension's own icon. */
    class Item(
        @JvmField val key: Int,
        @JvmField val title: String,
        @JvmField @DrawableRes val icon: Int,
        @JvmField val enabled: Boolean,
        @JvmField val bitmap: Bitmap?,
        @JvmField val submenu: Boolean,
    )

    var open by mutableStateOf(false)
    /** The link's text, or the image's title; the page's title for the page's own menu. */
    var title by mutableStateOf("")
    var url by mutableStateOf("")
    var image by mutableStateOf<Bitmap?>(null)
    var groups by mutableStateOf<List<List<Item>>>(emptyList())
    /** In a submenu: its title (a back row leads out of it). */
    var submenuTitle by mutableStateOf("")
}
