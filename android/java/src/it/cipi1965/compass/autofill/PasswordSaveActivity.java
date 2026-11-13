// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.autofill;

import android.app.Activity;
import android.os.Bundle;
import android.widget.Toast;

import org.chromium.chrome.browser.init.ChromeBrowserInitializer;
import org.chromium.chrome.browser.profiles.ProfileManager;

/**
 * Saves a login another app's form submitted (CompassAutofillService's save request, after the
 * user picked Save in Android's prompt), into Compass's passwords. It has no window of its own.
 */
public class PasswordSaveActivity extends Activity {
    static final String EXTRA_SIGNON_REALM = "it.cipi1965.compass.autofill.SIGNON_REALM";
    static final String EXTRA_USERNAME = "it.cipi1965.compass.autofill.USERNAME";
    static final String EXTRA_PASSWORD = "it.cipi1965.compass.autofill.PASSWORD";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String realm = getIntent().getStringExtra(EXTRA_SIGNON_REALM);
        String username = getIntent().getStringExtra(EXTRA_USERNAME);
        String password = getIntent().getStringExtra(EXTRA_PASSWORD);
        if (realm == null || password == null || password.isEmpty()) {
            finish();
            return;
        }
        try {
            // Compass's browser process, if this is all that runs of it.
            ChromeBrowserInitializer.getInstance().handleSynchronousStartup();
        } catch (RuntimeException e) {
            Toast.makeText(this, "Compass couldn't save the password", Toast.LENGTH_SHORT).show();
            finish();
            return;
        }
        PasswordsBridge bridge =
                new PasswordsBridge(ProfileManager.getLastUsedRegularProfile(), json -> {});
        bridge.save(realm, username != null ? username : "", password);
        bridge.destroy();
        Toast.makeText(this, "Saved to Compass passwords", Toast.LENGTH_SHORT).show();
        finish();
    }
}
