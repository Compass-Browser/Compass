// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONArray
import org.json.JSONObject

/**
 * The live folder editor's state (LiveFoldersUi.kt): New Live Folder (pick a plugin, then its
 * form) and Edit Source, and installing a plugin from its manifest's address. The form comes from
 * the plugin's manifest, as on the desktop (resources/sidebar/live_folders/editor.ts): its config
 * fields, the secrets it asks for (kept on this device) and, if it wants them, consent to send the
 * profile's cookies to its hosts. The JSON is SpacesBridge's.
 */
class LiveFoldersState {
    class Option(@JvmField val value: String, @JvmField val label: String)

    class Field(
        @JvmField val key: String,
        /** "string", "url", "number", "bool" or "select". */
        @JvmField val type: String,
        @JvmField val label: String,
        @JvmField val placeholder: String,
        @JvmField val optional: Boolean,
        /** The manifest's default, as text ("true"/"false" for a bool); null if none. */
        @JvmField val defaultValue: String?,
        @JvmField val options: List<Option>,
    )

    class Secret(
        @JvmField val key: String,
        @JvmField val label: String,
        @JvmField val help: String,
        @JvmField val optional: Boolean,
        /** One is stored on this device already. */
        @JvmField val set: Boolean,
    )

    class Plugin(
        @JvmField val id: String,
        @JvmField val name: String,
        @JvmField val description: String,
        @JvmField val builtin: Boolean,
        @JvmField val local: Boolean,
        @JvmField val config: List<Field>,
        @JvmField val secrets: List<Secret>,
        @JvmField val hosts: List<String>,
        @JvmField val cookieHosts: List<String>,
    )

    /** A plugin to install, for review. */
    class Preview(
        @JvmField val id: String,
        @JvmField val name: String,
        @JvmField val description: String,
        @JvmField val version: String,
        @JvmField val hosts: List<String>,
        @JvmField val cookieHosts: List<String>,
        @JvmField val secrets: List<String>,
        @JvmField val source: String,
        @JvmField val sha256: String,
    )

    var open by mutableStateOf(false)
    /** The folder Edit Source edits; empty for a new folder. */
    var folderId by mutableStateOf("")
    var loading by mutableStateOf(false)
    var plugins by mutableStateOf<List<Plugin>>(emptyList())
    var plugin by mutableStateOf<Plugin?>(null)
    var title by mutableStateOf("")
    /** What the user entered, by field key ("true"/"false" for a bool). */
    val values = mutableStateMapOf<String, String>()
    val secrets = mutableStateMapOf<String, String>()
    var grantCookies by mutableStateOf(false)
    var error by mutableStateOf("")
    var saving by mutableStateOf(false)

    /** Installing a plugin: its address, then what it is. */
    var installing by mutableStateOf(false)
    var installUrl by mutableStateOf("")
    var preview by mutableStateOf<Preview?>(null)
    /** The edited folder's plugin isn't on this device: where it comes from, and its hash. */
    var missingPluginUrl by mutableStateOf("")
    var missingPluginSha256 = ""

    val editing: Boolean
        get() = folderId.isNotEmpty()

    /** Starts New Live Folder (|folderId| empty) or Edit Source. */
    fun start(folderId: String) {
        this.folderId = folderId
        plugin = null
        title = ""
        values.clear()
        secrets.clear()
        grantCookies = false
        error = ""
        saving = false
        installing = false
        installUrl = ""
        preview = null
        missingPluginUrl = ""
        missingPluginSha256 = ""
        loading = true
    }

    fun pick(plugin: Plugin?) {
        this.plugin = plugin
        values.clear()
        secrets.clear()
        grantCookies = false
        error = ""
    }

    fun heading(): String =
        when {
            installing -> "Install a Plugin"
            editing -> "Edit Source"
            else -> plugin?.name ?: "New Live Folder"
        }

    /** The field's value: what the user entered, else the plugin's default. */
    fun value(field: Field): String = values[field.key] ?: field.defaultValue ?: ""

    fun setPluginsJson(json: String) {
        plugins = parseArray(json).map { parsePlugin(it) }
        loading = false
    }

    /** Fills the form from a folder's source (SpacesBridge.liveFolderSource()). */
    fun setSourceJson(json: String) {
        if (json.isEmpty()) {
            error = "This folder isn't live anymore."
            return
        }
        val source = JSONObject(json)
        val id = source.optString("pluginId")
        val found = plugins.firstOrNull { it.id == id }
        if (found == null) {
            error = "This folder's plugin isn't installed on this device."
            missingPluginUrl = source.optString("pluginUrl")
            missingPluginSha256 = source.optString("pluginSha256")
            return
        }
        plugin = found
        error = ""
        title = source.optString("title")
        grantCookies = source.optBoolean("cookiesGranted")
        val config = source.optJSONObject("config") ?: JSONObject()
        values.clear()
        for (field in found.config) {
            if (config.has(field.key) && !config.isNull(field.key)) {
                values[field.key] = textOf(config.get(field.key))
            }
        }
    }

    /** {"preview": …} or {"error": …} (SpacesBridge.previewLivePlugin()). */
    fun setPreviewJson(json: String) {
        val data = JSONObject(json)
        val p = data.optJSONObject("preview")
        if (p == null) {
            error = data.optString("error", "The plugin couldn't be read.")
            preview = null
            return
        }
        preview =
            Preview(
                p.optString("id"),
                p.optString("name"),
                p.optString("description"),
                p.optString("version"),
                strings(p.optJSONArray("hosts")),
                strings(p.optJSONArray("cookieHosts")),
                strings(p.optJSONArray("secrets")),
                p.optString("source"),
                p.optString("sha256"),
            )
        error =
            if (missingPluginSha256.isNotEmpty() && preview?.sha256 != missingPluginSha256) {
                "The plugin at that address isn't the one this folder was made with: it has " +
                    "changed since. Review it before installing."
            } else {
                ""
            }
    }

    /** The config object to save: the fields the user filled in, as their types. */
    fun configJson(): String {
        val plugin = plugin ?: return "{}"
        val config = JSONObject()
        for (field in plugin.config) {
            val value = values[field.key] ?: continue
            if (value.isEmpty()) continue
            when (field.type) {
                "bool" -> config.put(field.key, value == "true")
                "number" -> {
                    val number = value.trim().toDoubleOrNull()
                    when {
                        number == null -> config.put(field.key, value)
                        number == Math.floor(number) && !number.isInfinite() && Math.abs(number) < 1e15 ->
                            config.put(field.key, number.toLong())
                        else -> config.put(field.key, number)
                    }
                }
                else -> config.put(field.key, value)
            }
        }
        return config.toString()
    }

    fun secretsJson(): String {
        val json = JSONObject()
        for ((key, value) in secrets) if (value.isNotEmpty()) json.put(key, value)
        return json.toString()
    }

    companion object {
        /** "{config:url}" reads as the address the user enters. */
        @JvmStatic
        fun describeHosts(hosts: List<String>): String {
            if (hosts.isEmpty()) return "nothing"
            return hosts.map { if (it.startsWith("{")) "the address you enter" else it }.distinct().joinToString(", ")
        }

        private fun textOf(value: Any?): String =
            when (value) {
                null, JSONObject.NULL -> ""
                is Double -> if (value == Math.floor(value) && !value.isInfinite()) value.toLong().toString() else value.toString()
                else -> value.toString()
            }

        private fun parseArray(json: String): List<JSONObject> =
            try {
                JSONArray(json).let { array -> (0 until array.length()).mapNotNull { array.optJSONObject(it) } }
            } catch (e: org.json.JSONException) {
                emptyList()
            }

        private fun objects(array: JSONArray?): List<JSONObject> =
            if (array == null) emptyList() else (0 until array.length()).mapNotNull { array.optJSONObject(it) }

        private fun strings(array: JSONArray?): List<String> =
            if (array == null) emptyList() else (0 until array.length()).map { array.optString(it) }

        private fun parsePlugin(it: JSONObject): Plugin =
            Plugin(
                it.optString("id"),
                it.optString("name"),
                it.optString("description"),
                it.optBoolean("builtin"),
                it.optBoolean("local"),
                objects(it.optJSONArray("config")).map { f ->
                    Field(
                        f.optString("key"),
                        f.optString("type", "string"),
                        f.optString("label"),
                        f.optString("placeholder"),
                        f.optBoolean("optional"),
                        if (f.has("default") && !f.isNull("default")) textOf(f.get("default")) else null,
                        objects(f.optJSONArray("options")).map { o -> Option(o.optString("value"), o.optString("label")) },
                    )
                },
                objects(it.optJSONArray("secrets")).map { s ->
                    Secret(
                        s.optString("key"),
                        s.optString("label"),
                        s.optString("help"),
                        s.optBoolean("optional"),
                        s.optBoolean("set"),
                    )
                },
                strings(it.optJSONArray("hosts")),
                strings(it.optJSONArray("cookieHosts")),
            )
    }
}
