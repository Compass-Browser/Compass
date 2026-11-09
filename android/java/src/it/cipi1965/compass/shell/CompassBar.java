// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ActivityTabProvider;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.browser_controls.BottomControlsLayer;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker.LayerScrollBehavior;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker.LayerType;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker.LayerVisibility;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.layouts.LayoutType;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.ui.appmenu.AppMenuHandler;
import org.chromium.url.GURL;

/**
 * Compass's bottom bar (CompassBarUi.kt) in Chrome's tabbed activity. It sits in the bottom
 * controls stack, so web content makes room for it, and follows the activity's current tab.
 *
 * <p>Shell spike (milestone C1): Chrome's own toolbar stays for now.
 */
public class CompassBar implements BottomControlsLayer, CompassBarUi.Actions {
    private static final int HEIGHT_DP = 64;

    private final ChromeTabbedActivity mActivity;
    private final BottomControlsStacker mBottomControls;
    private final CompassBarState mState = new CompassBarState();
    private final int mHeightPx;
    private @Nullable View mView;
    private @Nullable CommandBar mCommandBar;
    private @Nullable Integer mBackgroundColor;

    public CompassBar(ChromeTabbedActivity activity, BottomControlsStacker bottomControls) {
        mActivity = activity;
        mBottomControls = bottomControls;
        mHeightPx = Math.round(HEIGHT_DP * activity.getResources().getDisplayMetrics().density);
    }

    public void attach() {
        View view = CompassBarUi.createView(mActivity, mActivity, mActivity, mState, this);
        // Chrome's bottom layers (the chin, its bottom toolbar, the mini player) live at the
        // bottom of its coordinator, which the stack's offsets are relative to; Chrome pads it or
        // not for the system navigation bar.
        ViewGroup coordinator =
                mActivity.findViewById(
                        mActivity
                                .getResources()
                                .getIdentifier("coordinator", "id", mActivity.getPackageName()));
        CoordinatorLayout.LayoutParams params =
                new CoordinatorLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, mHeightPx);
        params.gravity = Gravity.START | Gravity.BOTTOM;
        coordinator.addView(view, params);
        mView = view;
        mCommandBar = new CommandBar(mActivity, coordinator);

        // Registers itself with the tab provider.
        new ActivityTabProvider.ActivityTabTabObserver(
                        mActivity.getActivityTabProvider(), true) {
                    @Override
                    protected void onObservingDifferentTab(@Nullable Tab tab) {
                        update(tab);
                    }

                    @Override
                    public void onUrlUpdated(Tab tab) {
                        update(tab);
                    }

                    @Override
                    public void onTitleUpdated(Tab tab) {
                        update(tab);
                    }

                    @Override
                    public void onLoadStopped(Tab tab, boolean toDifferentDocument) {
                        update(tab);
                    }
                };

        TabModel tabs = mActivity.getTabModelSelector().getModel(false);
        tabs.getTabCountSupplier()
                .addSyncObserverAndCall(count -> mState.setTabCount(tabs.getCount()));

        mBottomControls.addLayer(this);
        mBottomControls.requestLayerUpdate(false);
    }

    private void update(@Nullable Tab tab) {
        if (tab == null) {
            mState.setHost("");
            mState.setCanGoBack(false);
            mState.setCanGoForward(false);
            return;
        }
        GURL url = tab.getUrl();
        // The new tab page (chrome-native://newtab) shows the bar's placeholder.
        String host = url.getScheme().startsWith("chrome") ? "" : url.getHost();
        mState.setHost(host);
        mState.setCanGoBack(tab.canGoBack());
        mState.setCanGoForward(tab.canGoForward());
        mState.setIncognito(tab.isIncognito());
    }

    // CompassBarUi.Actions:

    @Override
    public void back() {
        Tab tab = mActivity.getActivityTabProvider().get();
        if (tab != null && tab.canGoBack()) tab.goBack();
    }

    @Override
    public void openAddress() {
        if (mCommandBar != null) mCommandBar.open(/* inNewTab= */ false);
    }

    @Override
    public void newTab() {
        if (mCommandBar != null) mCommandBar.open(/* inNewTab= */ true);
    }

    @Override
    public void showTabs() {
        mActivity.getLayoutManager().showLayout(LayoutType.HUB, /* animate= */ true);
    }

    @Override
    public void showMenu() {
        // Chrome's menu (settings, history, downloads, ...), until Compass has its own.
        AppMenuHandler menu = mActivity.getRootUiCoordinatorForTesting().getAppMenuHandler();
        if (menu != null && mView != null) menu.showAppMenu(mView, /* startDragging= */ false);
    }

    @Override
    public void backgroundColorChanged(int color) {
        if (mBackgroundColor != null && mBackgroundColor == color) return;
        mBackgroundColor = color;
        mBottomControls.requestLayerUpdate(false);
    }

    // BottomControlsLayer:

    @Override
    public @LayerType int getType() {
        return LayerType.COMPASS_BAR;
    }

    @Override
    public @LayerScrollBehavior int getScrollBehavior() {
        // Scrolling off needs a composited copy of the bar; later.
        return LayerScrollBehavior.NEVER_SCROLL_OFF;
    }

    @Override
    public int getHeight() {
        return mHeightPx;
    }

    @Override
    public @LayerVisibility int getLayerVisibility() {
        return LayerVisibility.VISIBLE;
    }

    @Override
    public void onBrowserControlsOffsetUpdate(int layerYOffset) {
        // Negative to move up; positive only mid-animation, as for Chrome's mini player.
        if (mView != null) mView.setTranslationY(Math.min(0, layerYOffset));
    }

    @Override
    public @Nullable Integer getBackgroundColor() {
        return mBackgroundColor;
    }
}
