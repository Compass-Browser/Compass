// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.extras;

import android.app.Activity;
import android.net.Uri;
import android.os.Bundle;

import org.chromium.build.annotations.Nullable;

import java.io.UnsupportedEncodingException;
import java.net.URLDecoder;

/**
 * A pairing invite from a desktop's QR code (docs/design/sync.md, "Pairing QR code"):
 * compass-pair:?server=<escaped URL>&code=<code>, which the camera opens in Compass. The sync
 * screen joins the chain with it.
 */
public class PairInviteActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Uri data = getIntent().getData();
        String server = data != null ? param(data, "server") : null;
        String code = data != null ? param(data, "code") : null;
        if (server != null && code != null) {
            startActivity(
                    CompassIntents.create(this, CompassIntents.ACTION_PAIR)
                            .putExtra(CompassIntents.EXTRA_SERVER, server)
                            .putExtra(CompassIntents.EXTRA_CODE, code));
        }
        finish();
    }

    /** A query parameter of an opaque URI ("compass-pair:?a=b"), which Uri won't read. */
    static @Nullable String param(Uri uri, String name) {
        String text = uri.toString();
        int query = text.indexOf('?');
        if (query < 0) return null;
        for (String pair : text.substring(query + 1).split("&")) {
            int eq = pair.indexOf('=');
            if (eq > 0 && pair.substring(0, eq).equals(name)) {
                String value;
                try {
                    value = URLDecoder.decode(pair.substring(eq + 1), "UTF-8");
                } catch (UnsupportedEncodingException | IllegalArgumentException e) {
                    return null;
                }
                return value.isEmpty() ? null : value;
            }
        }
        return null;
    }
}
