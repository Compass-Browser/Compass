// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.extras;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.Context;
import android.widget.RemoteViews;

/** A home screen widget: a search field that opens the command bar. */
public class SearchWidgetProvider extends AppWidgetProvider {
    @Override
    public void onUpdate(Context context, AppWidgetManager manager, int[] widgetIds) {
        PendingIntent open =
                PendingIntent.getActivity(
                        context,
                        0,
                        CompassIntents.create(context, CompassIntents.ACTION_COMMAND_BAR),
                        PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        int layout =
                context.getResources()
                        .getIdentifier("compass_search_widget", "layout", context.getPackageName());
        int root =
                context.getResources()
                        .getIdentifier("compass_search_widget_root", "id", context.getPackageName());
        for (int id : widgetIds) {
            RemoteViews views = new RemoteViews(context.getPackageName(), layout);
            views.setOnClickPendingIntent(root, open);
            manager.updateAppWidget(id, views);
        }
    }
}
