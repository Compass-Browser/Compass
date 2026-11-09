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
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tabmodel.TabClosingSource;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.tabmodel.TabModelObserver;

import java.util.ArrayList;
import java.util.List;

/**
 * The toast after closing tabs (UndoToastUi.kt), in place of Chrome's undo snackbar (the
 * UndoBarController patch keeps that away): a closure the tab model holds pending shows it; Undo
 * cancels the closure, and once the toast is gone it's committed. A new toast ends the one before.
 */
public class UndoToast implements UndoToastUi.Listener {
    private static final long DURATION_MS = 4000;

    private final UndoToastUi.State mState = new UndoToastUi.State();
    private final View mView;
    private final List<TabModel> mModels = new ArrayList<>();
    // The closure on show: its model and tabs.
    private @Nullable TabModel mModel;
    private final List<Integer> mTabIds = new ArrayList<>();
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
        for (boolean incognito : new boolean[] {false, true}) {
            TabModel model = activity.getTabModelSelector().getModel(incognito);
            android.util.Log.w(
                    "CompassUndo",
                    "observing " + model + " pending " + model.supportsPendingClosures());
            TabModelObserver observer =
                    new TabModelObserver() {
                        @Override
                        public void onTabClosePending(
                                List<Tab> tabs,
                                boolean isAllTabs,
                                @TabClosingSource int closingSource) {
                            android.util.Log.w("CompassUndo", "pending " + tabs.size());
                            show(model, tabs);
                        }
                    };
            model.addObserver(observer);
            mModels.add(model);
            mObservers.add(observer);
        }
    }

    private final List<TabModelObserver> mObservers = new ArrayList<>();

    private void show(TabModel model, List<Tab> tabs) {
        if (tabs.isEmpty()) return;
        // The toast before: its tabs are closed now.
        finish(false);
        mModel = model;
        for (Tab tab : tabs) mTabIds.add(tab.getId());
        mState.setText(
                tabs.size() == 1
                        ? "Closed “" + tabs.get(0).getTitle() + "”"
                        : "Closed " + tabs.size() + " tabs");
        mState.setShown(true);
        mView.bringToFront();
        mView.removeCallbacks(mTimeout);
        mView.postDelayed(mTimeout, DURATION_MS);
    }

    public void destroy() {
        finish(false);
        for (int i = 0; i < mModels.size(); i++) mModels.get(i).removeObserver(mObservers.get(i));
        mModels.clear();
        mObservers.clear();
    }

    private void finish(boolean undo) {
        mView.removeCallbacks(mTimeout);
        TabModel model = mModel;
        mModel = null;
        mState.setShown(false);
        if (model != null) {
            for (int id : mTabIds) {
                // Chrome may have ended it already (the app went to the background).
                if (!model.isClosurePending(id)) continue;
                if (undo) {
                    model.cancelTabClosure(id);
                } else {
                    model.commitTabClosure(id);
                }
            }
        }
        mTabIds.clear();
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
