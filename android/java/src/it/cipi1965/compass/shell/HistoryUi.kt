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
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R
import java.util.Calendar
import java.util.Date

/**
 * History, as the iOS app's (HistoryView): a search field over the visits, newest first, under
 * their day; a tap opens one, × removes it. Clear Browsing Data is Chrome's page.
 */
object HistoryUi {
    /** A visit (Chrome's HistoryItem): |index| is its place in HistoryScreen's list. */
    class Visit(
        @JvmField val index: Int,
        @JvmField val title: String,
        @JvmField val url: String,
        @JvmField val host: String,
        /** Milliseconds since the epoch. */
        @JvmField val time: Long,
    )

    /** A day's visits, under its heading. */
    class Day(@JvmField val title: String, @JvmField val visits: List<Visit>)

    interface Listener {
        fun onQuery(query: String)

        fun onOpen(visit: Visit)

        fun onRemove(visit: Visit)

        /** The end of the list is on screen: the next page, if there's one. */
        fun onMore()

        fun onClearData()

        fun onDismiss()
    }

    class State {
        var open by mutableStateOf(false)
        var query by mutableStateOf("")
        var visits by mutableStateOf<List<Visit>>(emptyList())
        var loading by mutableStateOf(false)
        var hasMore by mutableStateOf(false)
    }

    /**
     * |visits| (newest first) under their days: "Today", "Yesterday", then the date (|dateTitle|),
     * by the calendar of |now|.
     */
    @JvmStatic
    fun byDay(visits: List<Visit>, now: Calendar, dateTitle: (Long) -> String): List<Day> {
        fun dayStart(calendar: Calendar): Long {
            val day = calendar.clone() as Calendar
            day.set(Calendar.HOUR_OF_DAY, 0)
            day.set(Calendar.MINUTE, 0)
            day.set(Calendar.SECOND, 0)
            day.set(Calendar.MILLISECOND, 0)
            return day.timeInMillis
        }
        val today = dayStart(now)
        val yesterdayCalendar = now.clone() as Calendar
        yesterdayCalendar.add(Calendar.DAY_OF_YEAR, -1)
        val yesterday = dayStart(yesterdayCalendar)
        val days = ArrayList<Day>()
        var title = ""
        var current = ArrayList<Visit>()
        for (visit in visits) {
            val visitTitle =
                when {
                    visit.time >= today -> "Today"
                    visit.time >= yesterday -> "Yesterday"
                    else -> dateTitle(visit.time)
                }
            if (visitTitle != title && current.isNotEmpty()) {
                days.add(Day(title, current))
                current = ArrayList()
            }
            title = visitTitle
            current.add(visit)
        }
        if (current.isNotEmpty()) days.add(Day(title, current))
        return days
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
                if (state.open) History(context, state, listener)
            }
        }
        return view
    }
}

@androidx.compose.runtime.Composable
private fun History(context: Context, state: HistoryUi.State, listener: HistoryUi.Listener) {
    BackHandler { listener.onDismiss() }
    val dateFormat = remember { DateFormat.getLongDateFormat(context) }
    val timeFormat = remember { DateFormat.getTimeFormat(context) }
    val days = HistoryUi.byDay(state.visits, Calendar.getInstance()) { dateFormat.format(Date(it)) }
    val list = rememberLazyListState()
    val atEnd by remember {
        derivedStateOf {
            val last = list.layoutInfo.visibleItemsInfo.lastOrNull()?.index ?: 0
            last >= list.layoutInfo.totalItemsCount - 3
        }
    }
    LaunchedEffect(atEnd, state.hasMore, state.loading) {
        if (atEnd && state.hasMore && !state.loading) listener.onMore()
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
                    "History",
                    style = MaterialTheme.typography.titleLarge,
                    modifier = Modifier.padding(start = 4.dp).weight(1f),
                )
                TextButton(onClick = { listener.onClearData() }) { Text("Clear…") }
            }
            @Suppress("DEPRECATION") TextField(
                value = state.query,
                onValueChange = { listener.onQuery(it) },
                placeholder = { Text("Search history") },
                leadingIcon = { Sym(R.drawable.compass_sym_search, 18f, MaterialTheme.colorScheme.onSurfaceVariant) },
                singleLine = true,
                shape = RoundedCornerShape(28.dp),
                colors =
                    TextFieldDefaults.colors(
                        focusedIndicatorColor = Color.Transparent,
                        unfocusedIndicatorColor = Color.Transparent,
                        focusedContainerColor = MaterialTheme.colorScheme.surfaceContainerHigh,
                        unfocusedContainerColor = MaterialTheme.colorScheme.surfaceContainerHigh,
                    ),
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            )
            if (days.isEmpty() && !state.loading) {
                Text(
                    if (state.query.isEmpty()) "No history" else "Nothing found",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 24.dp, vertical = 24.dp),
                )
            }
            LazyColumn(
                Modifier.fillMaxSize(),
                state = list,
                contentPadding = PaddingValues(start = 16.dp, end = 16.dp, bottom = 24.dp),
            ) {
                for (day in days) {
                    item(key = "day:" + day.title) {
                        Text(
                            day.title,
                            style = MaterialTheme.typography.titleSmall,
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.padding(start = 8.dp, top = 16.dp, bottom = 8.dp),
                        )
                    }
                    items(day.visits, key = { "visit:${it.index}:${it.time}:${it.url}" }) { visit ->
                        Row(
                            Modifier.fillMaxWidth()
                                .heightIn(min = 64.dp)
                                .clip(RoundedCornerShape(16.dp))
                                .clickable { listener.onOpen(visit) }
                                .padding(start = 8.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(16.dp),
                        ) {
                            SiteTile(visit.url)
                            Column(Modifier.weight(1f)) {
                                Text(
                                    visit.title.ifEmpty { visit.url },
                                    style = MaterialTheme.typography.bodyLarge,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                                Text(
                                    "${visit.host} · ${timeFormat.format(Date(visit.time))}",
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            }
                            Box(
                                Modifier.size(48.dp).clip(CircleShape).clickable { listener.onRemove(visit) },
                                contentAlignment = Alignment.Center,
                            ) {
                                Sym(R.drawable.compass_sym_close, 18f, MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                }
            }
        }
    }
}
