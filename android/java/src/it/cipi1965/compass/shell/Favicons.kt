// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.graphics.Bitmap
import android.net.Uri
import androidx.compose.runtime.mutableStateMapOf
import org.chromium.chrome.browser.profiles.Profile
import org.chromium.chrome.browser.ui.favicon.FaviconHelper
import org.chromium.url.GURL

/**
 * Site icons for the shell's views (SiteIcon), by host, from Chrome's favicon database: asked for
 * on first use, kept for the process, and replaced when a tab's page brings a new one.
 */
object Favicons {
    private const val SIZE_PX = 64

    private val icons = mutableStateMapOf<String, Bitmap>()
    private val asked = HashSet<String>()
    private var helper: FaviconHelper? = null
    private var profile: Profile? = null

    /** Once the browser is up (CompassBar). */
    @JvmStatic
    fun init(profile: Profile) {
        this.profile = profile
        if (helper == null) helper = FaviconHelper()
    }

    /** A tab's page brought its icon. */
    @JvmStatic
    fun put(url: String, icon: Bitmap?) {
        val host = hostOf(url)
        if (icon != null && host.isNotEmpty()) icons[host] = icon
    }

    fun hostOf(url: String): String = runCatching { Uri.parse(url).host }.getOrNull()?.lowercase() ?: ""

    /** |url|'s site icon, or null for now (it comes later, as state). */
    fun icon(url: String): Bitmap? {
        val host = hostOf(url)
        if (host.isEmpty()) return null
        icons[host]?.let { return it }
        val helper = helper
        val profile = profile
        if (helper != null && profile != null && asked.add(host)) {
            val gurl = GURL(url)
            if (gurl.isValid) {
                helper.getLocalFaviconImageForURL(profile, gurl, SIZE_PX, /* fallbackToHost= */ true) { image, _ ->
                    if (image != null) icons[host] = image
                }
            }
        }
        return null
    }
}
