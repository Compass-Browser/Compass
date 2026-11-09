// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.content.res.Configuration;
import android.graphics.Color;
import android.view.View;

import it.cipi1965.compass.spaces.SpacesBridge;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tabmodel.TabClosureParams;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.ui.native_page.NativePage;
import org.chromium.components.embedder_support.util.UrlConstants;

/**
 * Compass's start page (StartPageUi.kt), in place of Chrome's new tab page for regular tabs.
 * Chrome's NativePageFactory asks the shell for it (patch). As on the desktop, a tab on it has no
 * sidebar entry until it goes somewhere (SpacesAndroid).
 */
public class StartPage implements NativePage, StartPageUi.Listener {
    private final ChromeTabbedActivity mActivity;
    private final Tab mTab;
    private final SpacesBridge mBridge;
    private final SidebarState mState = new SidebarState();
    private final View mView;
    private int mBackgroundColor;

    public StartPage(ChromeTabbedActivity activity, Tab tab) {
        mActivity = activity;
        mTab = tab;
        boolean night =
                (activity.getResources().getConfiguration().uiMode
                                & Configuration.UI_MODE_NIGHT_MASK)
                        == Configuration.UI_MODE_NIGHT_YES;
        mBackgroundColor = night ? Color.BLACK : Color.WHITE;
        mBridge = new SpacesBridge(tab.getProfile(), this::refresh);
        refresh();
        mView = StartPageUi.createView(activity, activity, activity, mState, this);
    }

    private void refresh() {
        mState.update(mBridge.snapshot(), mTab.getId());
    }

    // StartPageUi.Listener:

    @Override
    public void onSearch() {
        CompassBar bar = CompassBar.forActivity(mActivity);
        // For this tab, as Chrome's search box on its new tab page.
        if (bar != null) bar.openAddress();
    }

    @Override
    public void onOpenNode(String nodeId, String url) {
        CompassBar bar = CompassBar.forActivity(mActivity);
        if (bar == null) return;
        // The entry's tab, or a new one for it: this one is no longer needed.
        bar.openNode(nodeId, url);
        // After the click: closing the tab destroys this page.
        mView.post(
                () -> {
                    TabModel model = mActivity.getTabModelSelector().getModel(false);
                    if (model.getTabById(mTab.getId()) == null || model.getCount() < 2) return;
                    model.getTabRemover()
                            .closeTabs(
                                    TabClosureParams.closeTab(mTab).allowUndo(false).build(),
                                    /* allowDialog= */ false);
                });
    }

    @Override
    public void onBackgroundColor(int color) {
        mBackgroundColor = color;
    }

    // NativePage:

    @Override
    public @Nullable View getView() {
        return mView;
    }

    @Override
    public String getTitle() {
        return "New tab";
    }

    @Override
    public String getUrl() {
        return UrlConstants.NTP_URL;
    }

    @Override
    public String getHost() {
        return UrlConstants.NTP_HOST;
    }

    @Override
    public int getBackgroundColor() {
        return mBackgroundColor;
    }

    @Override
    public boolean needsToolbarShadow() {
        return false;
    }

    @Override
    public void updateForUrl(String url) {}

    @Override
    public void destroy() {
        mBridge.destroy();
    }
}
