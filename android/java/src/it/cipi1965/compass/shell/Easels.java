// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.app.AlertDialog;
import android.widget.Toast;

import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.content_public.browser.WebContents;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * New Easel and Capture to Easel (docs/design/android.md, "Easels"). A capture takes the whole
 * visible page: dragging a region on a phone fights the page's own scrolling, and the board's
 * editor crops an image. Refresh is the desktop's for now.
 */
@NullMarked
final class Easels {
    private Easels() {}

    /** A board with its tab pinned in the window's space, opened. */
    static void newEasel(ChromeTabbedActivity activity, Spaces spaces) {
        String result =
                EaselBridge.newEasel(
                        activity.getTabModelSelector().getModel(false).getProfile(),
                        spaces.shownSpaceId());
        try {
            JSONObject json = new JSONObject(result);
            String error = json.optString("error");
            if (!error.isEmpty()) {
                toast(activity, error);
                return;
            }
            spaces.onNodeClicked(json.getString("nodeId"), json.getString("url"));
        } catch (JSONException e) {
            toast(activity, "The Easel couldn't be made.");
        }
    }

    /** Whether |tab|'s page can be captured: a web page, not incognito. */
    static boolean canCapture(@Nullable Tab tab) {
        if (tab == null || tab.isIncognito() || tab.getWebContents() == null) return false;
        String scheme = tab.getUrl().getScheme();
        return scheme.equals("http") || scheme.equals("https") || scheme.equals("file");
    }

    /** Asks for the board (the last used first, then New Easel), then adds the visible page. */
    static void capture(ChromeTabbedActivity activity, @Nullable Spaces spaces) {
        Tab tab = activity.getActivityTab();
        if (tab == null || !canCapture(tab)) {
            toast(activity, "Only web pages can be captured.");
            return;
        }
        String spaceId = spaces != null ? spaces.shownSpaceId() : "";
        String[] ids;
        String[] titles;
        try {
            JSONArray boards = new JSONArray(EaselBridge.easels(tab.getProfile()));
            ids = new String[boards.length() + 1];
            titles = new String[boards.length() + 1];
            for (int i = 0; i < boards.length(); i++) {
                JSONObject board = boards.getJSONObject(i);
                ids[i] = board.getString("id");
                titles[i] = board.getString("title");
            }
        } catch (JSONException e) {
            return;
        }
        ids[ids.length - 1] = "";
        titles[titles.length - 1] = "New Easel";
        new AlertDialog.Builder(activity)
                .setTitle("Capture to Easel")
                .setItems(
                        titles,
                        (dialog, which) -> {
                            WebContents contents = tab.getWebContents();
                            if (contents == null || contents.isDestroyed()) return;
                            EaselBridge.capture(
                                    contents,
                                    ids[which],
                                    spaceId,
                                    result -> onCaptured(activity, result));
                        })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private static void onCaptured(ChromeTabbedActivity activity, String result) {
        try {
            JSONObject json = new JSONObject(result);
            String error = json.optString("error");
            toast(activity, error.isEmpty() ? "Added to “" + json.optString("title") + "”" : error);
        } catch (JSONException e) {
            toast(activity, "The page couldn't be captured.");
        }
    }

    private static void toast(ChromeTabbedActivity activity, String text) {
        Toast.makeText(activity, text, Toast.LENGTH_SHORT).show();
    }
}
