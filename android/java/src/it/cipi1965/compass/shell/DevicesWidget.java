// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.content.Context;

import it.cipi1965.compass.extras.DevicesWidgetProvider;
import it.cipi1965.compass.extras.ListWidgetProvider;

import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.recent_tabs.ForeignSessionHelper;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * Keeps the Other Devices widget (DevicesWidgetProvider) up to date while Compass runs: the sync
 * chain's other devices' open tabs, from Chrome's ForeignSessionHelper, as sync brings them.
 */
public class DevicesWidget {
    private final Context mContext;
    private final ForeignSessionHelper mSessions;

    public DevicesWidget(Context context, Profile profile) {
        mContext = context;
        mSessions = new ForeignSessionHelper(profile);
        mSessions.setOnForeignSessionCallback(this::publish);
        publish();
    }

    public void destroy() {
        mSessions.destroy();
    }

    private void publish() {
        try {
            JSONArray rows = new JSONArray();
            if (mSessions.isTabSyncEnabled()) {
                List<ForeignSessionHelper.ForeignSession> sessions =
                        new ArrayList<>(mSessions.getForeignSessions());
                // The most recently active device first.
                sessions.sort((a, b) -> Long.compare(b.modifiedTime, a.modifiedTime));
                outer:
                for (ForeignSessionHelper.ForeignSession session : sessions) {
                    for (ForeignSessionHelper.ForeignSessionWindow window : session.windows) {
                        for (ForeignSessionHelper.ForeignSessionTab tab : window.tabs) {
                            if (!tab.url.isValid()) continue;
                            String title = tab.title.isEmpty() ? tab.url.getSpec() : tab.title;
                            rows.put(
                                    new JSONObject()
                                            .put("title", title + " · " + session.name)
                                            .put("url", tab.url.getSpec()));
                            if (rows.length() == ListWidgetProvider.rows()) break outer;
                        }
                    }
                }
            }
            ListWidgetProvider.publish(
                    mContext,
                    new DevicesWidgetProvider(),
                    new JSONObject().put("title", "Other Devices").put("rows", rows).toString());
        } catch (JSONException e) {
            // Not published.
        }
    }
}
