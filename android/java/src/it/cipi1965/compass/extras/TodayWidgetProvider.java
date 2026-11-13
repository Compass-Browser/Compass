// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.extras;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.view.View;
import android.widget.RemoteViews;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * A home screen widget, as the iOS app's Today widget: the active space's Today entries, the
 * first few; a tap opens one. The shell publishes them as they change (publish()), so the widget
 * shows them while Compass isn't running.
 */
public class TodayWidgetProvider extends AppWidgetProvider {
    private static final String PREFS = "compass_widgets";
    private static final String KEY_TODAY = "today";
    private static final int ROWS = 4;

    /**
     * Stores what the widget shows and updates it: |json| is {"space": name, "entries": [{"id",
     * "title", "url"}]}. Does nothing if it's what the widget already shows.
     */
    public static void publish(Context context, String json) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (json.equals(prefs.getString(KEY_TODAY, ""))) return;
        prefs.edit().putString(KEY_TODAY, json).apply();
        AppWidgetManager manager = AppWidgetManager.getInstance(context);
        int[] ids =
                manager.getAppWidgetIds(new ComponentName(context, TodayWidgetProvider.class));
        if (ids.length > 0) update(context, manager, ids, json);
    }

    @Override
    public void onUpdate(Context context, AppWidgetManager manager, int[] widgetIds) {
        String json =
                context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                        .getString(KEY_TODAY, "");
        update(context, manager, widgetIds, json);
    }

    private static void update(
            Context context, AppWidgetManager manager, int[] widgetIds, String json) {
        RemoteViews views =
                new RemoteViews(
                        context.getPackageName(),
                        CompassIntents.resource(context, "layout", "compass_today_widget"));
        String space = "";
        JSONArray entries = new JSONArray();
        try {
            JSONObject data = new JSONObject(json);
            space = data.optString("space");
            JSONArray list = data.optJSONArray("entries");
            if (list != null) entries = list;
        } catch (JSONException e) {
            // Nothing published yet.
        }
        views.setTextViewText(
                CompassIntents.resource(context, "id", "compass_today_title"),
                space.isEmpty() ? "Today" : space + " · Today");
        // The title opens the sidebar.
        views.setOnClickPendingIntent(
                CompassIntents.resource(context, "id", "compass_today_title"),
                PendingIntent.getActivity(
                        context,
                        200,
                        CompassIntents.create(context, CompassIntents.ACTION_SIDEBAR),
                        PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        views.setViewVisibility(
                CompassIntents.resource(context, "id", "compass_today_empty"),
                entries.length() == 0 ? View.VISIBLE : View.GONE);
        for (int i = 0; i < ROWS; i++) {
            int row = CompassIntents.resource(context, "id", "compass_today_row" + i);
            JSONObject entry = entries.optJSONObject(i);
            if (entry == null) {
                views.setViewVisibility(row, View.GONE);
                continue;
            }
            String url = entry.optString("url");
            String title = entry.optString("title");
            views.setViewVisibility(row, View.VISIBLE);
            views.setTextViewText(row, title.isEmpty() ? url : title);
            Intent open = CompassIntents.create(context, CompassIntents.ACTION_OPEN_ENTRY);
            open.putExtra(CompassIntents.EXTRA_NODE_ID, entry.optString("id"));
            open.putExtra(CompassIntents.EXTRA_URL, url);
            views.setOnClickPendingIntent(
                    row,
                    PendingIntent.getActivity(
                            context,
                            210 + i,
                            open,
                            PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        }
        for (int id : widgetIds) manager.updateAppWidget(id, views);
    }
}
