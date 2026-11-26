// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.extras;

import android.content.Context;

import it.cipi1965.compass.R;

/** A home screen widget, as the iOS app's Today widget: the active space's Today entries. */
public class TodayWidgetProvider extends ListWidgetProvider {
    @Override
    protected String key() {
        return "today";
    }

    @Override
    protected String prefix() {
        return "compass_today_";
    }

    @Override
    protected String defaultTitle(Context context) {
        return context.getString(R.string.compass_widget_today);
    }
}
