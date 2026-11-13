// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.tab.Tab;

import java.util.Collections;

/**
 * The page menu (PageMenuUi.kt) in place of Chrome's app menu, from the bar's ⋯. What it does is
 * the bar's (CompassBar), or Chrome's own (find, share, downloads, history, page info, settings).
 */
public class PageMenu implements PageMenuUi.Listener {
    private final ChromeTabbedActivity mActivity;
    private final CompassBar mBar;
    private final PageMenuState mState = new PageMenuState();
    private final View mView;

    public PageMenu(ChromeTabbedActivity activity, ViewGroup parent, CompassBar bar) {
        mActivity = activity;
        mBar = bar;
        mView = PageMenuUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    public void show() {
        Tab tab = mActivity.getActivityTab();
        boolean page = tab != null && !tab.getUrl().getScheme().startsWith("chrome");
        mState.setUrl(page ? tab.getUrl().getSpec() : "");
        mState.setHost(page ? tab.getUrl().getHost() : "");
        mState.setCanGoBack(tab != null && tab.canGoBack());
        mState.setCanGoForward(tab != null && tab.canGoForward());
        mState.setIncognito(tab != null && tab.isIncognito());
        mState.setPinned(tab != null && mBar.pinnedNodeFor(tab.getId()) != null);
        mState.setTint(mBar.spaceTint());
        mState.setExtensions(
                tab != null && tab.isIncognito() ? Collections.emptyList() : mBar.extensionActions());
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
    }

    /** Runs |action| once the sheet is gone (another sheet or screen follows). */
    private void then(Runnable action) {
        onDismiss();
        action.run();
    }

    // PageMenuUi.Listener:

    @Override
    public void onBack() {
        then(mBar::goBack);
    }

    @Override
    public void onForward() {
        then(mBar::goForward);
    }

    @Override
    public void onEditAddress() {
        then(mBar::openAddress);
    }

    @Override
    public void onCopyUrl() {
        // The sheet stays: the link turns into a checkmark.
        mBar.copyUrl();
    }

    @Override
    public void onReload() {
        then(mBar::reload);
    }

    @Override
    public void onFind() {
        then(mBar::findInPage);
    }

    @Override
    public void onAskClaude() {
        then(mBar::showClaude);
    }

    @Override
    public void onTogglePin() {
        then(mBar::togglePin);
    }

    @Override
    public void onShare() {
        then(mBar::share);
    }

    @Override
    public void onDownloads() {
        then(() -> mBar.chromeAction("downloads_menu_id"));
    }

    @Override
    public void onHistory() {
        then(mBar::showHistory);
    }

    @Override
    public void onRunExtension(String id) {
        then(() -> mBar.runExtension(id));
    }

    @Override
    public void onExtensions() {
        then(mBar::showExtensions);
    }

    @Override
    public void onSiteSettings() {
        then(() -> mBar.chromeAction("info_menu_id"));
    }

    @Override
    public void onSettings() {
        then(mBar::showSettings);
    }

    @Override
    public void onDismiss() {
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
