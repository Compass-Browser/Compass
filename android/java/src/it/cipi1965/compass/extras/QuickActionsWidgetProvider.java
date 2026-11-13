// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.extras;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.Context;
import android.widget.RemoteViews;

/** A home screen widget, as the iOS app's Quick Actions: search, Incognito, Claude, the sidebar. */
public class QuickActionsWidgetProvider extends AppWidgetProvider {
    @Override
    public void onUpdate(Context context, AppWidgetManager manager, int[] widgetIds) {
        RemoteViews views =
                new RemoteViews(
                        context.getPackageName(),
                        CompassIntents.resource(context, "layout", "compass_actions_widget"));
        String[][] buttons = {
            {"compass_actions_search", CompassIntents.ACTION_COMMAND_BAR},
            {"compass_actions_incognito", CompassIntents.ACTION_INCOGNITO},
            {"compass_actions_claude", CompassIntents.ACTION_CLAUDE},
            {"compass_actions_sidebar", CompassIntents.ACTION_SIDEBAR},
        };
        for (int i = 0; i < buttons.length; i++) {
            PendingIntent intent =
                    PendingIntent.getActivity(
                            context,
                            // One request code per action, so they don't replace each other.
                            100 + i,
                            CompassIntents.create(context, buttons[i][1]),
                            PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            views.setOnClickPendingIntent(
                    CompassIntents.resource(context, "id", buttons[i][0]), intent);
        }
        for (int id : widgetIds) manager.updateAppWidget(id, views);
    }
}
