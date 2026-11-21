// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.extras;

import android.app.Activity;
import android.net.Uri;
import android.os.Bundle;

/**
 * An invite link to a shared space, folder or easel (docs/design/sync.md, "Rooms: clients"):
 * https://compassbrowser.org/join#… or compass://join#…, from another app or the camera. The
 * shell's join screen (ShareSheet) previews it; its fragment never reaches a server.
 */
public class JoinRoomActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Uri data = getIntent().getData();
        if (data != null && data.getFragment() != null && !data.getFragment().isEmpty()) {
            // The canonical form, which the link parser takes either way.
            String link = "https://compassbrowser.org/join#" + data.getEncodedFragment();
            startActivity(
                    CompassIntents.create(this, CompassIntents.ACTION_JOIN_ROOM)
                            .putExtra(CompassIntents.EXTRA_URL, link));
        }
        finish();
    }
}
