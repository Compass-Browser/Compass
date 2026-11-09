// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.claude.ClaudeBridge;

import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.tab.Tab;

/** The Claude chat (ClaudeChatUi.kt) over ClaudeBridge, about the tab on screen. */
public class ClaudeChat implements ClaudeChatUi.Listener {
    private final ChromeTabbedActivity mActivity;
    private final ClaudeBridge mBridge;
    private final ClaudeChatState mState = new ClaudeChatState();
    private final View mView;

    public ClaudeChat(ChromeTabbedActivity activity, ViewGroup parent) {
        mActivity = activity;
        mBridge =
                new ClaudeBridge(
                        activity.getTabModelSelector().getModel(false).getProfile(),
                        this::refresh);
        mView = ClaudeChatUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    public void destroy() {
        mBridge.destroy();
    }

    public void show() {
        refresh();
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
    }

    private void refresh() {
        mState.update(mBridge.snapshot());
    }

    @Override
    public void onSend(String text) {
        Tab tab = mActivity.getActivityTabProvider().get();
        // Incognito pages stay out of it.
        mBridge.send(text, tab != null && !tab.isIncognito() ? tab.getWebContents() : null);
    }

    @Override
    public void onStop() {
        mBridge.stop();
    }

    @Override
    public void onClear() {
        mBridge.clear();
    }

    @Override
    public void onSetKey(String key) {
        mBridge.setApiKey(key);
    }

    @Override
    public void onForgetKey() {
        // An empty key removes the saved one (ClaudeCredentials).
        mBridge.setApiKey("");
    }

    @Override
    public void onDismiss() {
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
