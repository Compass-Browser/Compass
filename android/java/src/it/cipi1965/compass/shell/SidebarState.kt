// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONArray
import org.json.JSONObject

/** What the sidebar shows: the snapshot from SpacesAndroid (spaces_android.h), parsed. */
class SidebarState {
    class Space(
        @JvmField val id: String,
        @JvmField val name: String,
        /** "#rrggbb", or empty for grey. */
        @JvmField val tint: String,
        @JvmField val emoji: String,
    )

    class Node(
        @JvmField val id: String,
        @JvmField val type: String,
        @JvmField val title: String,
        @JvmField val url: String,
        @JvmField val collapsed: Boolean,
        /** The node's tab, or -1. */
        @JvmField val tab: Int,
        @JvmField val children: List<Node>,
        /** A live folder's plugin, or empty (docs/design/live-folders.md). */
        @JvmField val live: String = "",
        /** A live folder's entry that came with a refresh and hasn't been opened. */
        @JvmField val unseen: Boolean = false,
        /** A live folder's plugin's name; empty if this device doesn't have the plugin. */
        @JvmField val liveName: String = "",
        /** A live folder whose plugin isn't on this device. */
        @JvmField val liveMissing: Boolean = false,
        /** A live folder that's refreshing. */
        @JvmField val refreshing: Boolean = false,
        /** Why a live folder's last refresh failed, or empty. */
        @JvmField val error: String = "",
    ) {
        val isFolder: Boolean
            get() = type == "folder"
        val isLiveFolder: Boolean
            get() = isFolder && live.isNotEmpty()
        /** A live folder's entries that haven't been opened. */
        val unseenCount: Int
            get() = children.count { it.unseen }
    }

    var open by mutableStateOf(false)
    /** An incognito tab's page (the start page): no spaces. */
    var incognito by mutableStateOf(false)
    var loaded by mutableStateOf(false)
    var spaces by mutableStateOf<List<Space>>(emptyList())
    var activeId by mutableStateOf("")
    var favorites by mutableStateOf<List<Node>>(emptyList())
    var pinned by mutableStateOf<List<Node>>(emptyList())
    var today by mutableStateOf<List<Node>>(emptyList())
    /** The tab on screen, to highlight its entry. */
    var currentTab by mutableStateOf(-1)

    /** The pinned entry or favorite showing |tab|, or null (a Today tab, or none). */
    fun pinnedNodeFor(tab: Int): Node? {
        fun find(nodes: List<Node>): Node? {
            for (node in nodes) {
                if (node.tab == tab) return node
                find(node.children)?.let { return it }
            }
            return null
        }
        return if (tab < 0) null else find(favorites) ?: find(pinned)
    }

    val activeSpace: Space?
        get() = spaces.firstOrNull { it.id == activeId }

    fun update(json: String, currentTabId: Int) {
        val data = JSONObject(json)
        loaded = data.optBoolean("loaded")
        activeId = data.optString("active")
        spaces =
            data.optJSONArray("spaces").objects().map {
                Space(
                    it.optString("id"),
                    it.optString("name"),
                    it.optString("tint"),
                    it.optString("emoji"),
                )
            }
        favorites = nodes(data.optJSONArray("favorites"))
        pinned = nodes(data.optJSONArray("pinned"))
        today = nodes(data.optJSONArray("today"))
        currentTab = currentTabId
    }

    private fun nodes(array: JSONArray?): List<Node> =
        array.objects().map {
            Node(
                it.optString("id"),
                it.optString("type"),
                it.optString("title"),
                it.optString("url"),
                it.optBoolean("collapsed"),
                it.optInt("tab", -1),
                nodes(it.optJSONArray("children")),
                it.optString("live"),
                it.optBoolean("unseen"),
                it.optString("live_name"),
                it.optBoolean("live_missing"),
                it.optBoolean("refreshing"),
                it.optString("error"),
            )
        }

    private fun JSONArray?.objects(): List<JSONObject> =
        if (this == null) emptyList() else (0 until length()).mapNotNull { optJSONObject(it) }
}
