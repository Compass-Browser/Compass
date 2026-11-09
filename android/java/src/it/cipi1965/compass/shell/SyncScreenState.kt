// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject

/** What the sync screen shows: SyncBridge's snapshot (sync_bridge.h), parsed. */
class SyncScreenState {
    class Device(@JvmField val name: String, @JvmField val kind: String, @JvmField val self: Boolean)

    class DataType(@JvmField val type: Int, @JvmField val enabled: Boolean)

    var open by mutableStateOf(false)
    /** "unavailable", "off", "needs_restart", "starting", "on" or "error". */
    var state by mutableStateOf("off")
    var server by mutableStateOf("")
    var message by mutableStateOf("")
    /** "none", "working", "showing_code", "confirm", "waiting", "done" or "failed". */
    var pairingStep by mutableStateOf("none")
    var pairingCode by mutableStateOf("")
    var pairingDevice by mutableStateOf("")
    var pairingMessage by mutableStateOf("")
    var devices by mutableStateOf<List<Device>>(emptyList())
    var types by mutableStateOf<List<DataType>>(emptyList())
    var error by mutableStateOf("")
    var newWords by mutableStateOf<List<String>>(emptyList())

    fun update(json: String) {
        val data = JSONObject(json)
        state = data.optString("state", "off")
        server = data.optString("server")
        message = data.optString("message")
        val pairing = data.optJSONObject("pairing")
        pairingStep = pairing?.optString("step", "none") ?: "none"
        pairingCode = pairing?.optString("code") ?: ""
        pairingDevice = pairing?.optString("device") ?: ""
        pairingMessage = pairing?.optString("message") ?: ""
        val deviceArray = data.optJSONArray("devices")
        devices =
            (0 until (deviceArray?.length() ?: 0)).mapNotNull { i ->
                deviceArray?.optJSONObject(i)?.let {
                    Device(it.optString("name"), it.optString("kind"), it.optBoolean("this"))
                }
            }
        val typeArray = data.optJSONArray("types")
        types =
            (0 until (typeArray?.length() ?: 0)).mapNotNull { i ->
                typeArray?.optJSONObject(i)?.let { DataType(it.optInt("type"), it.optBoolean("enabled")) }
            }
        error = data.optString("error")
        val words = data.optJSONArray("new_words")
        newWords = (0 until (words?.length() ?: 0)).map { words!!.optString(it) }
    }
}
