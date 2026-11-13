// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.text.format.DateFormat
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.compose.BackHandler
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R
import java.util.Date
import java.util.Locale

/**
 * Downloads, in the shell's style, in place of Chrome's download home: newest first, with the
 * progress of those under way (pause, resume, cancel); a tap opens a finished one, a long press
 * deletes it. Chrome's download settings stay behind Settings.
 */
object DownloadsUi {
    /** What a download is doing (Chrome's OfflineItemState, simplified). */
    enum class Status {
        RUNNING,
        PAUSED,
        DONE,
        FAILED,
    }

    class Download(
        @JvmField val key: String,
        @JvmField val title: String,
        @JvmField val host: String,
        @JvmField val status: Status,
        @JvmField val receivedBytes: Long,
        /** 0 when unknown. */
        @JvmField val totalBytes: Long,
        /** 0 to 100, or -1 when unknown. */
        @JvmField val percent: Int,
        /** Milliseconds since the epoch. */
        @JvmField val time: Long,
    )

    interface Listener {
        fun onOpen(key: String)

        fun onPauseOrResume(key: String)

        fun onCancel(key: String)

        fun onDelete(key: String)

        fun onSettings()

        fun onDismiss()
    }

    class State {
        var open by mutableStateOf(false)
        var downloads by mutableStateOf<List<Download>>(emptyList())
    }

    /** "512 B", "1.5 KB", "12 MB", "1.2 GB": a size the way Android's Files shows it. */
    @JvmStatic
    fun formatSize(bytes: Long): String {
        if (bytes < 1000) return "$bytes B"
        val units = listOf("KB", "MB", "GB", "TB")
        var value = bytes / 1000.0
        var unit = 0
        while (value >= 1000 && unit < units.size - 1) {
            value /= 1000
            unit++
        }
        return if (value < 10) String.format(Locale.US, "%.1f %s", value, units[unit])
        else String.format(Locale.US, "%.0f %s", value, units[unit])
    }

    /** The line under a download's name: its progress, or its size and site. */
    @JvmStatic
    fun subtitle(download: Download): String {
        val sizes =
            if (download.totalBytes > 0) "${formatSize(download.receivedBytes)} of ${formatSize(download.totalBytes)}"
            else formatSize(download.receivedBytes)
        return when (download.status) {
            Status.RUNNING -> if (download.percent >= 0) "${download.percent}% · $sizes" else sizes
            Status.PAUSED -> "Paused · $sizes"
            Status.FAILED -> "Failed"
            Status.DONE ->
                listOf(formatSize(maxOf(download.totalBytes, download.receivedBytes)), download.host)
                    .filter { it.isNotEmpty() }
                    .joinToString(" · ")
        }
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: State,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) {
                if (state.open) Downloads(context, state, listener)
            }
        }
        return view
    }
}

@OptIn(ExperimentalFoundationApi::class)
@androidx.compose.runtime.Composable
private fun Downloads(context: Context, state: DownloadsUi.State, listener: DownloadsUi.Listener) {
    BackHandler { listener.onDismiss() }
    val dateFormat = remember { DateFormat.getMediumDateFormat(context) }
    var deleting by remember { mutableStateOf<DownloadsUi.Download?>(null) }
    deleting?.let { download ->
        AlertDialog(
            onDismissRequest = { deleting = null },
            title = { Text("Delete download?") },
            text = { Text("\"${download.title}\" will be deleted from this device.") },
            confirmButton = {
                TextButton(onClick = {
                    deleting = null
                    listener.onDelete(download.key)
                }) { Text("Delete", color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = { TextButton(onClick = { deleting = null }) { Text("Cancel") } },
        )
    }
    Box(
        Modifier.fillMaxSize()
            .background(MaterialTheme.colorScheme.surface)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {}
            .windowInsetsPadding(WindowInsets.safeDrawing)
    ) {
        Column(Modifier.fillMaxSize()) {
            Row(
                Modifier.fillMaxWidth().height(64.dp).padding(horizontal = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(
                    Modifier.size(48.dp).clip(CircleShape).clickable { listener.onDismiss() },
                    contentAlignment = Alignment.Center,
                ) {
                    Sym(R.drawable.compass_sym_arrow_back, 20f)
                }
                Text(
                    "Downloads",
                    style = MaterialTheme.typography.titleLarge,
                    modifier = Modifier.padding(start = 4.dp).weight(1f),
                )
                TextButton(onClick = { listener.onSettings() }) { Text("Settings") }
            }
            if (state.downloads.isEmpty()) {
                Text(
                    "No downloads",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 24.dp, vertical = 24.dp),
                )
            }
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 24.dp),
            ) {
                items(state.downloads, key = { it.key }) { download ->
                    Row(
                        Modifier.fillMaxWidth()
                            .heightIn(min = 72.dp)
                            .clip(RoundedCornerShape(16.dp))
                            .combinedClickable(
                                onClick = {
                                    if (download.status == DownloadsUi.Status.DONE) listener.onOpen(download.key)
                                },
                                onLongClick = {
                                    if (download.status == DownloadsUi.Status.DONE) deleting = download
                                },
                            )
                            .padding(start = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(16.dp),
                    ) {
                        MenuIcon(R.drawable.compass_sym_download)
                        Column(Modifier.weight(1f)) {
                            Text(
                                download.title,
                                style = MaterialTheme.typography.bodyLarge,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                            val subtitle = DownloadsUi.subtitle(download)
                            Text(
                                if (download.status == DownloadsUi.Status.DONE && download.time > 0)
                                    "$subtitle · ${dateFormat.format(Date(download.time))}"
                                else subtitle,
                                style = MaterialTheme.typography.bodyMedium,
                                color =
                                    if (download.status == DownloadsUi.Status.FAILED) MaterialTheme.colorScheme.error
                                    else MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                            if (download.status == DownloadsUi.Status.RUNNING || download.status == DownloadsUi.Status.PAUSED) {
                                if (download.percent >= 0) {
                                    LinearProgressIndicator(
                                        progress = { download.percent / 100f },
                                        modifier = Modifier.fillMaxWidth().padding(top = 6.dp),
                                    )
                                } else {
                                    LinearProgressIndicator(Modifier.fillMaxWidth().padding(top = 6.dp))
                                }
                            }
                        }
                        when (download.status) {
                            DownloadsUi.Status.RUNNING, DownloadsUi.Status.PAUSED -> {
                                TextButton(onClick = { listener.onPauseOrResume(download.key) }) {
                                    Text(if (download.status == DownloadsUi.Status.PAUSED) "Resume" else "Pause")
                                }
                                Box(
                                    Modifier.size(48.dp).clip(CircleShape).clickable { listener.onCancel(download.key) },
                                    contentAlignment = Alignment.Center,
                                ) {
                                    Sym(R.drawable.compass_sym_close, 18f, MaterialTheme.colorScheme.onSurfaceVariant)
                                }
                            }
                            DownloadsUi.Status.FAILED -> {
                                Box(
                                    Modifier.size(48.dp).clip(CircleShape).clickable { listener.onDelete(download.key) },
                                    contentAlignment = Alignment.Center,
                                ) {
                                    Sym(R.drawable.compass_sym_close, 18f, MaterialTheme.colorScheme.onSurfaceVariant)
                                }
                            }
                            DownloadsUi.Status.DONE -> {}
                        }
                    }
                }
            }
        }
    }
}
