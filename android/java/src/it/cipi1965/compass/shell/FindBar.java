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
import org.chromium.chrome.browser.tab.TabObserver;
import org.chromium.components.find_in_page.FindInPageBridge;
import org.chromium.components.find_in_page.FindNotificationDetails;

/**
 * Find on page (FindBarUi.kt) over Chrome's find in page (FindInPageBridge), for the tab on
 * screen; it ends when the tab changes or Done is tapped.
 */
public class FindBar implements FindBarUi.Listener {
    private final ChromeTabbedActivity mActivity;
    private final FindBarUi.State mState = new FindBarUi.State();
    private final View mView;
    private @Nullable Tab mTab;
    private @Nullable FindInPageBridge mBridge;

    private final TabObserver mObserver =
            new TabObserver() {
                @Override
                public void onFindResultAvailable(FindNotificationDetails result) {
                    mState.setAnswered(true);
                    mState.setTotal(result.numberOfMatches);
                    mState.setActive(Math.max(0, result.activeMatchOrdinal));
                }

                @Override
                public void onHidden(Tab tab, int reason) {
                    onDone();
                }
            };

    public FindBar(ChromeTabbedActivity activity, ViewGroup parent) {
        mActivity = activity;
        mView = FindBarUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        CoordinatorLayout.LayoutParams params =
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        // At the bottom, over the bar; the window shrinks above the keyboard.
        params.gravity = Gravity.BOTTOM;
        parent.addView(mView, params);
    }

    public void show() {
        Tab tab = mActivity.getActivityTab();
        if (tab == null || tab.getWebContents() == null) return;
        if (mTab != tab) {
            onDone();
            mTab = tab;
            mBridge = new FindInPageBridge(tab.getWebContents());
            tab.addObserver(mObserver);
        }
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
    }

    @Override
    public void onTextChanged(String text) {
        mState.setText(text);
        mState.setAnswered(false);
        if (mBridge == null) return;
        if (text.isEmpty()) {
            mBridge.stopFinding(true);
            mState.setTotal(0);
            mState.setActive(0);
        } else {
            mBridge.startFinding(text, true, false);
        }
    }

    @Override
    public void onNext(boolean forward) {
        if (mBridge != null && !mState.getText().isEmpty()) {
            mBridge.startFinding(mState.getText(), forward, false);
        }
    }

    @Override
    public void onDone() {
        if (mBridge != null) {
            mBridge.stopFinding(true);
            mBridge.destroy();
            mBridge = null;
        }
        if (mTab != null) {
            mTab.removeObserver(mObserver);
            mTab = null;
        }
        mState.setOpen(false);
        mState.setText("");
        mState.setTotal(0);
        mState.setActive(0);
        mView.setVisibility(View.GONE);
    }
}
