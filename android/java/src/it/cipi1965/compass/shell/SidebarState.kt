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
        /** Its pinned tree's root, and that tree's folders (live ones aside), for Pin to Compass. */
        @JvmField val pinnedId: String = "",
        @JvmField val folders: List<Folder> = emptyList(),
    )

    /** A folder of a space's pinned tree; |depth| 0 is in the root. */
    class Folder(@JvmField val id: String, @JvmField val title: String, @JvmField val depth: Int)

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
        /** A pinned tab's or favorite's pinned URL, or empty (docs/design/sidebar.md, "Pinned URL"). */
        @JvmField val savedUrl: String = "",
    ) {
        val isFolder: Boolean
            get() = type == "folder"
        val isLiveFolder: Boolean
            get() = isFolder && live.isNotEmpty()
        /** A pinned tab or favorite that's on another page than its pinned URL. */
        val isAwayFromSavedUrl: Boolean
            get() = type == "tab" && savedUrl.isNotEmpty() && url != savedUrl
    }

    /** An archived Today entry: a tab, or a split's panes (title: theirs, joined). */
    class ArchivedEntry(
        @JvmField val id: String,
        @JvmField val title: String,
        @JvmField val url: String,
        /** Milliseconds since the epoch. */
        @JvmField val archivedAt: Long,
    )

    var open by mutableStateOf(false)
    /** An incognito tab's page (the start page): no spaces. */
    var incognito by mutableStateOf(false)
    var loaded by mutableStateOf(false)
    var spaces by mutableStateOf<List<Space>>(emptyList())
    var activeId by mutableStateOf("")
    var favorites by mutableStateOf<List<Node>>(emptyList())
    var pinned by mutableStateOf<List<Node>>(emptyList())
    var today by mutableStateOf<List<Node>>(emptyList())
    /** The roots' IDs (pinned and Today of the shown space, the favorites), for drops. */
    var pinnedId by mutableStateOf("")
    var todayId by mutableStateOf("")
    var favoritesId by mutableStateOf("")
    /** The shown space's archive, newest first. */
    var archive by mutableStateOf<List<ArchivedEntry>>(emptyList())
    /** How long the shown space's Today entries stay unseen before the archive; 0: never. */
    var archiveAfterHours by mutableStateOf(12)
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
                    it.optString("pinned_id"),
                    it.optJSONArray("folders").objects().map { folder ->
                        Folder(folder.optString("id"), folder.optString("title"), folder.optInt("depth"))
                    },
                )
            }
        favorites = nodes(data.optJSONArray("favorites"))
        pinned = nodes(data.optJSONArray("pinned"))
        today = nodes(data.optJSONArray("today"))
        pinnedId = data.optString("pinned_id")
        todayId = data.optString("today_id")
        favoritesId = data.optString("favorites_id")
        archive =
            data.optJSONArray("archive").objects().map {
                ArchivedEntry(
                    it.optString("id"),
                    it.optString("title"),
                    it.optString("url"),
                    it.optDouble("archived_at", 0.0).toLong(),
                )
            }
        archiveAfterHours = data.optInt("archive_after_hours", 12)
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
                it.optString("saved_url"),
            )
        }

    private fun JSONArray?.objects(): List<JSONObject> =
        if (this == null) emptyList() else (0 until length()).mapNotNull { optJSONObject(it) }
}
