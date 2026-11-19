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
        /** Its whole theme: colors, gradient, intensity, grain, mode (SpaceTheme.kt). */
        @JvmField val theme: SpaceTheme = SpaceTheme.GREY,
        /** Its profile's name, and whether it's this window's profile (docs/design/android.md, "Profiles"). */
        @JvmField val profile: String = "",
        @JvmField val own: Boolean = true,
        /** Its profile's directory, for the window that shows it. */
        @JvmField val profileDir: String = "",
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
        /** Claude's short title, which the row shows (Shorter Tab Titles); empty: none. */
        @JvmField val shortTitle: String = "",
        /** A folder Claude named, and whether Undo Name can take its name back. */
        @JvmField val claudeName: Boolean = false,
        @JvmField val canUndoName: Boolean = false,
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
    /** This window's profile's spaces. */
    var spaces by mutableStateOf<List<Space>>(emptyList())
    /** Every profile's spaces, in the browser-wide order (the switcher's row). */
    var allSpaces by mutableStateOf<List<Space>>(emptyList())
    /** How many profiles have spaces: more than one shows profile names. */
    var profileCount by mutableStateOf(1)
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
    /** The tab Claude acts on while it works (its outline in the sidebar), or -1. */
    var claudeTab by mutableStateOf(-1)

    // Claude in the sidebar (docs/design/ai-sidebar.md), from ClaudeSidebarBridge's snapshot.
    /** Suggest a Name in a folder's menu ("Name Link Folders" on, Claude set up). */
    var suggestNames by mutableStateOf(false)
    /** Tidy in Today's header ("Tidy Today" on, Claude set up). */
    var tidyAvailable by mutableStateOf(false)
    /** A tidy of the shown space waits for Claude. */
    var tidyBusy by mutableStateOf(false)
    /** The folder Claude just named, which sparkles for a moment; empty: none. */
    var sparkleId by mutableStateOf("")
    /**
     * Tidy Today's result while the sidebar is open (it's a window of its own, over the toast):
     * the text, and whether it has Undo; empty: none.
     */
    var toast by mutableStateOf("")
    var toastUndoable by mutableStateOf(false)

    /** Today's loose entries (tabs and splits at its top), which Tidy groups; at least 5 to tidy. */
    val looseTodayEntries: Int
        get() = today.count { it.type == "tab" || it.type == "split" }

    /** The split one of whose panes shows |tab| (in Pinned or Today), or null. */
    fun splitOf(tab: Int): Node? {
        fun find(nodes: List<Node>): Node? {
            for (node in nodes) {
                if (node.type == "split" && node.children.any { it.tab == tab }) return node
                find(node.children)?.let { return it }
            }
            return null
        }
        return if (tab < 0) null else find(pinned) ?: find(today)
    }

    /** The node |id| of the shown space or the favorites, or null. */
    fun findNode(id: String): Node? {
        fun find(nodes: List<Node>): Node? {
            for (node in nodes) {
                if (node.id == id) return node
                find(node.children)?.let { return it }
            }
            return null
        }
        return find(pinned) ?: find(today) ?: find(favorites)
    }

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
        allSpaces =
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
                    SpaceTheme.fromJson(it),
                    it.optString("profile"),
                    it.optBoolean("own", true),
                    it.optString("profile_dir"),
                )
            }
        spaces = allSpaces.filter { it.own }
        profileCount = data.optInt("profile_count", 1)
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
                it.optString("short_title"),
                it.optBoolean("claude_name"),
                it.optBoolean("can_undo_name"),
            )
        }

    private fun JSONArray?.objects(): List<JSONObject> =
        if (this == null) emptyList() else (0 until length()).mapNotNull { optJSONObject(it) }
}
