// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.extras;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Patterns;

import java.util.regex.Matcher;

/** The share target "Pin to Compass": opens the shared link and pins it to the active space. */
public class PinShareActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String text = getIntent().getStringExtra(Intent.EXTRA_TEXT);
        if (text != null) {
            // Apps often share "Title https://...": take the link.
            Matcher matcher = Patterns.WEB_URL.matcher(text);
            if (matcher.find()) {
                Intent intent = CompassIntents.create(this, CompassIntents.ACTION_PIN_URL);
                intent.putExtra(CompassIntents.EXTRA_URL, matcher.group());
                startActivity(intent);
            }
        }
        finish();
    }
}
