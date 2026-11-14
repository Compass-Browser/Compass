// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.extras;

import android.app.PendingIntent;
import android.content.Intent;
import android.os.Build;
import android.service.quicksettings.TileService;

/**
 * A Quick Settings tile: a new tab, with the command bar open. In :base_java, the app bundle's base
 * module, as the system binds services by their class (b/169196314).
 */
public class NewTabTileService extends TileService {
    @Override
    @SuppressWarnings("deprecation") // startActivityAndCollapse(Intent), before Android 14.
    public void onClick() {
        Intent intent = CompassIntents.create(this, CompassIntents.ACTION_COMMAND_BAR);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startActivityAndCollapse(
                    PendingIntent.getActivity(
                            this,
                            0,
                            intent,
                            PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        } else {
            startActivityAndCollapse(intent);
        }
    }
}
