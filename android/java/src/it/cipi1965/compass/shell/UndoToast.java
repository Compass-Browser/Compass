// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;

/**
 * The toast after closing tabs (UndoToastUi.kt), in place of Chrome's undo snackbar
 * (UndoBarController patch): Undo reopens them, and once it's gone they're closed for good. A new
 * toast ends the one before.
 */
public class UndoToast implements UndoToastUi.Listener {
    private static final long DURATION_MS = 4000;

    private final UndoToastUi.State mState = new UndoToastUi.State();
    private final View mView;
    private @Nullable Runnable mUndo;
    private @Nullable Runnable mCommit;
    private final Runnable mTimeout = this::onDismiss;

    /** |aboveBarPx|: how far over the bottom it floats (the bar's height). */
    public UndoToast(ChromeTabbedActivity activity, ViewGroup parent, int aboveBarPx) {
        mView = UndoToastUi.createView(activity, activity, activity, mState, this);
        CoordinatorLayout.LayoutParams params =
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.gravity = Gravity.BOTTOM;
        params.bottomMargin = aboveBarPx;
        parent.addView(mView, params);
    }

    public void show(String text, Runnable undo, Runnable commit) {
        // The toast before: its tabs are closed now.
        finish(false);
        mUndo = undo;
        mCommit = commit;
        mState.setText(text);
        mState.setShown(true);
        org.chromium.base.Log.i("UndoToast", "showing %s", text);
        mView.bringToFront();
        mView.removeCallbacks(mTimeout);
        mView.postDelayed(mTimeout, DURATION_MS);
    }

    public void destroy() {
        finish(false);
    }

    private void finish(boolean undo) {
        mView.removeCallbacks(mTimeout);
        Runnable action = undo ? mUndo : mCommit;
        mUndo = null;
        mCommit = null;
        mState.setShown(false);
        if (action != null) action.run();
    }

    @Override
    public void onUndo() {
        finish(true);
    }

    @Override
    public void onDismiss() {
        finish(false);
    }
}
