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
 * A home screen widget listing pages (Today, Other Devices): a title and the first few rows; a tap
 * opens one. The shell publishes the rows as they change (publish()), so the widget shows them
 * while Compass isn't running. Its layout is "<prefix>widget" with the views "<prefix>root",
 * "<prefix>title", "<prefix>empty" and "<prefix>row0" to "<prefix>row3".
 */
public abstract class ListWidgetProvider extends AppWidgetProvider {
    private static final String PREFS = "compass_widgets";
    private static final int ROWS = 4;

    /** Where the rows are kept, in the widgets' preferences. */
    protected abstract String key();

    /** The layout's and its views' names' prefix ("compass_today_"). */
    protected abstract String prefix();

    /** The title when nothing was published yet. */
    protected abstract String defaultTitle();

    /** The number of rows the widget shows. */
    public static int rows() {
        return ROWS;
    }

    /**
     * Stores what |widget|'s kind shows and updates them: |json| is {"title", "rows": [{"id",
     * "title", "url"}]} (an "id" is a Today entry's). Does nothing if it's what they already show.
     */
    public static void publish(Context context, ListWidgetProvider widget, String json) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (json.equals(prefs.getString(widget.key(), ""))) return;
        prefs.edit().putString(widget.key(), json).apply();
        AppWidgetManager manager = AppWidgetManager.getInstance(context);
        int[] ids = manager.getAppWidgetIds(new ComponentName(context, widget.getClass()));
        if (ids.length > 0) widget.update(context, manager, ids, json);
    }

    @Override
    public void onUpdate(Context context, AppWidgetManager manager, int[] widgetIds) {
        String json =
                context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(key(), "");
        update(context, manager, widgetIds, json);
    }

    private int id(Context context, String name) {
        return CompassIntents.resource(context, "id", prefix() + name);
    }

    private void update(Context context, AppWidgetManager manager, int[] widgetIds, String json) {
        RemoteViews views =
                new RemoteViews(
                        context.getPackageName(),
                        CompassIntents.resource(context, "layout", prefix() + "widget"));
        String title = "";
        JSONArray rows = new JSONArray();
        try {
            JSONObject data = new JSONObject(json);
            title = data.optString("title");
            JSONArray list = data.optJSONArray("rows");
            if (list != null) rows = list;
        } catch (JSONException e) {
            // Nothing published yet.
        }
        views.setTextViewText(id(context, "title"), title.isEmpty() ? defaultTitle() : title);
        // The title opens the sidebar.
        views.setOnClickPendingIntent(
                id(context, "title"),
                PendingIntent.getActivity(
                        context,
                        requestCode(0),
                        CompassIntents.create(context, CompassIntents.ACTION_SIDEBAR),
                        PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        views.setViewVisibility(
                id(context, "empty"), rows.length() == 0 ? View.VISIBLE : View.GONE);
        for (int i = 0; i < ROWS; i++) {
            int row = id(context, "row" + i);
            JSONObject entry = rows.optJSONObject(i);
            if (entry == null) {
                views.setViewVisibility(row, View.GONE);
                continue;
            }
            String url = entry.optString("url");
            String text = entry.optString("title");
            views.setViewVisibility(row, View.VISIBLE);
            views.setTextViewText(row, text.isEmpty() ? url : text);
            Intent open = CompassIntents.create(context, CompassIntents.ACTION_OPEN_ENTRY);
            open.putExtra(CompassIntents.EXTRA_NODE_ID, entry.optString("id"));
            open.putExtra(CompassIntents.EXTRA_URL, url);
            views.setOnClickPendingIntent(
                    row,
                    PendingIntent.getActivity(
                            context,
                            requestCode(1 + i),
                            open,
                            PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        }
        for (int widgetId : widgetIds) manager.updateAppWidget(widgetId, views);
    }

    // Each widget kind's pending intents apart from the others'.
    private int requestCode(int index) {
        return 200 + (prefix().hashCode() & 0xff) * 16 + index;
    }
}
