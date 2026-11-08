// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.os.Build;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.FrameLayout;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ActivityTabProvider;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.browser_controls.BottomControlsLayer;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker.LayerScrollBehavior;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker.LayerType;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker.LayerVisibility;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.url.GURL;

/**
 * Compass's bottom bar (CompassBarUi.kt) in Chrome's tabbed activity. It sits in the bottom
 * controls stack, so web content makes room for it, and follows the activity's current tab.
 *
 * <p>Shell spike (milestone C1): Chrome's own toolbar stays for now.
 */
public class CompassBar implements BottomControlsLayer {
    private static final int HEIGHT_DP = 64;

    private final ChromeTabbedActivity mActivity;
    private final BottomControlsStacker mBottomControls;
    private final CompassBarState mState = new CompassBarState();
    private final int mHeightPx;
    private @Nullable View mView;
    // Where the stack puts the bar, and the system navigation bar it sits on.
    private int mLayerYOffset;
    private int mNavigationBarHeight;

    public CompassBar(ChromeTabbedActivity activity, BottomControlsStacker bottomControls) {
        mActivity = activity;
        mBottomControls = bottomControls;
        mHeightPx = Math.round(HEIGHT_DP * activity.getResources().getDisplayMetrics().density);
    }

    public void attach() {
        View view =
                CompassBarUi.createView(
                        mActivity,
                        mActivity,
                        mActivity,
                        mState,
                        this::goBack,
                        this::goForward,
                        this::openNewTab);
        ViewGroup content = mActivity.findViewById(android.R.id.content);
        content.addView(
                view,
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, mHeightPx, Gravity.BOTTOM));
        mView = view;
        // The content view runs under the system navigation bar (edge to edge); the stack's
        // offsets start above it.
        view.setOnApplyWindowInsetsListener(
                (v, insets) -> {
                    WindowInsets root = v.getRootWindowInsets();
                    mNavigationBarHeight = navigationBarHeight(root != null ? root : insets);
                    position();
                    return insets;
                });
        view.requestApplyInsets();

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

    private void goBack() {
        Tab tab = mActivity.getActivityTabProvider().get();
        if (tab != null && tab.canGoBack()) tab.goBack();
    }

    private void goForward() {
        Tab tab = mActivity.getActivityTabProvider().get();
        if (tab != null && tab.canGoForward()) tab.goForward();
    }

    private void openNewTab() {
        mActivity.getTabCreator(false).launchNtp(TabLaunchType.FROM_CHROME_UI);
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
        mLayerYOffset = layerYOffset;
        position();
    }

    private void position() {
        if (mView != null) mView.setTranslationY(mLayerYOffset - mNavigationBarHeight);
    }

    @SuppressWarnings("deprecation") // getSystemWindowInsetBottom(), before Android 11.
    private static int navigationBarHeight(WindowInsets insets) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return insets.getInsets(WindowInsets.Type.navigationBars()).bottom;
        }
        return insets.getSystemWindowInsetBottom();
    }
}
