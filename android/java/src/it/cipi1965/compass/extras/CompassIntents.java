// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.extras;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;

/**
 * Intents from Compass's extras (the Quick Settings tile, the widget, shortcuts, the share target)
 * to its browser, handled by the shell (CompassBar).
 */
public final class CompassIntents {
    /** Opens the command bar for a new tab. */
    public static final String ACTION_COMMAND_BAR = "it.cipi1965.compass.action.COMMAND_BAR";

    /** Shows a space ({@link #EXTRA_SPACE_ID}). */
    public static final String ACTION_OPEN_SPACE = "it.cipi1965.compass.action.OPEN_SPACE";

    /** Opens a page ({@link #EXTRA_URL}) and pins it to the active space. */
    public static final String ACTION_PIN_URL = "it.cipi1965.compass.action.PIN_URL";

    /** Opens a new Incognito tab. */
    public static final String ACTION_INCOGNITO = "it.cipi1965.compass.action.INCOGNITO";

    /** Opens Claude's chat. */
    public static final String ACTION_CLAUDE = "it.cipi1965.compass.action.CLAUDE";

    /** Shows the sidebar. */
    public static final String ACTION_SIDEBAR = "it.cipi1965.compass.action.SIDEBAR";

    /** Opens a Today entry ({@link #EXTRA_NODE_ID}), or its page ({@link #EXTRA_URL}) if it's gone. */
    public static final String ACTION_OPEN_ENTRY = "it.cipi1965.compass.action.OPEN_ENTRY";

    /** Joins a chain with a pairing invite ({@link #EXTRA_SERVER}, {@link #EXTRA_CODE}). */
    public static final String ACTION_PAIR = "it.cipi1965.compass.action.PAIR";

    public static final String EXTRA_SPACE_ID = "it.cipi1965.compass.extra.SPACE_ID";
    public static final String EXTRA_SERVER = "it.cipi1965.compass.extra.SERVER";
    public static final String EXTRA_CODE = "it.cipi1965.compass.extra.CODE";
    public static final String EXTRA_URL = "it.cipi1965.compass.extra.URL";
    public static final String EXTRA_NODE_ID = "it.cipi1965.compass.extra.NODE_ID";

    // The launcher alias of Chrome's tabbed activity, in every Compass build.
    private static final String MAIN_ACTIVITY = "com.google.android.apps.chrome.Main";

    private CompassIntents() {}

    /** An intent for the browser with |action|. */
    public static Intent create(Context context, String action) {
        Intent intent = new Intent(action);
        intent.setComponent(new ComponentName(context.getPackageName(), MAIN_ACTIVITY));
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return intent;
    }

    /** Whether |intent| is one of Compass's. */
    public static boolean isCompassIntent(Intent intent) {
        String action = intent.getAction();
        return ACTION_COMMAND_BAR.equals(action)
                || ACTION_OPEN_SPACE.equals(action)
                || ACTION_PIN_URL.equals(action)
                || ACTION_PAIR.equals(action)
                || ACTION_INCOGNITO.equals(action)
                || ACTION_CLAUDE.equals(action)
                || ACTION_SIDEBAR.equals(action)
                || ACTION_OPEN_ENTRY.equals(action);
    }

    /** One of the extras' resources, by type and name (they're in the APK's package). */
    public static int resource(Context context, String type, String name) {
        return context.getResources().getIdentifier(name, type, context.getPackageName());
    }
}
