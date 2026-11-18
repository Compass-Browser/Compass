// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.graphics.Color;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import org.chromium.base.Callback;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.browser_controls.TopControlLayer;
import org.chromium.chrome.browser.browser_controls.TopControlsStacker;
import org.chromium.chrome.browser.browser_controls.TopControlsStacker.TopControlType;
import org.chromium.chrome.browser.browser_controls.TopControlsStacker.TopControlVisibility;
import org.chromium.chrome.browser.compositor.CompositorViewHolder;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabObserver;
import org.chromium.chrome.browser.tab.TabSelectionType;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.tabmodel.TabModelUtils;
import org.chromium.components.embedder_support.view.ContentView;
import org.chromium.components.thinwebview.ThinWebView;
import org.chromium.components.thinwebview.ThinWebViewAttachParams;
import org.chromium.components.thinwebview.ThinWebViewConstraints;
import org.chromium.components.thinwebview.ThinWebViewFactory;
import org.chromium.content_public.browser.Visibility;
import org.chromium.content_public.browser.WebContents;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Splits (docs/design/android.md, "Splits"). The shown tab's entry is a pane of a split: a split
 * of two shows both panes, one above the other (side by side in landscape), each under a title
 * strip, the front one outlined; a tap on the other's strip brings it to the front. A split of
 * three or four shows one pane at a time under a pane picker.
 *
 * <p>Chrome draws one tab, the window's active one: the front pane. The space the other pane takes
 * is Chrome's browser controls' (a top layer here, CompassBar's bottom layer) or, side by side,
 * insets CompositorViewHolder leaves beside the page. The other pane's page is borrowed into a
 * ThinWebView there, which has a compositor of its own; it keeps its tab (its delegate, its
 * helpers, its place in the tab model) and goes back to the window when its tab is shown.
 */
public class SplitView implements SplitUi.Listener {
    private static final int STRIP_DP = 36;
    private static final int PICKER_DP = 52;

    private final ChromeTabbedActivity mActivity;
    private final ViewGroup mCoordinator;
    private final Spaces mSpaces;
    // CompassBar's bottom layer's height: Chrome's page ends above it.
    private final Callback<Integer> mBottomHeight;
    private final SplitUi.State mState = new SplitUi.State();
    private final View mOverlay;
    // The other pane's page.
    private final FrameLayout mHost;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final float mDensity;

    private int mTopHeight;
    private final TopControlLayer mTopLayer =
            new TopControlLayer() {
                @Override
                public @TopControlType int getTopControlType() {
                    return TopControlType.COMPASS_SPLIT;
                }

                @Override
                public int getTopControlHeight() {
                    return mTopHeight;
                }

                @Override
                public @TopControlVisibility int getTopControlVisibility() {
                    return mTopHeight > 0
                            ? TopControlVisibility.VISIBLE
                            : TopControlVisibility.HIDDEN;
                }

                @Override
                public @TopControlsStacker.ScrollBehavior int getScrollBehavior() {
                    return TopControlsStacker.ScrollBehavior.NEVER_SCROLLABLE;
                }
            };
    private final @Nullable TopControlsStacker mTopControls;

    private @Nullable Tab mBorrowed;
    private @Nullable ThinWebView mThinWebView;
    // Panes whose page is opening in a background tab, so it's asked once.
    private final Set<String> mOpening = new HashSet<>();
    private boolean mUpdatePosted;

    private final TabObserver mBorrowedObserver =
            new TabObserver() {
                // Chrome is about to show it: it goes back to the window first.
                @Override
                public void onShown(Tab tab, @TabSelectionType int type) {
                    release();
                }

                @Override
                public void onClosingStateChanged(Tab tab, boolean closing) {
                    if (closing) release();
                }

                @Override
                public void onDestroyed(Tab tab) {
                    release();
                }

                @Override
                public void onContentChanged(Tab tab) {
                    // A new WebContents (a crash, a prerender): borrow that one.
                    release();
                    postUpdate();
                }
            };

    public SplitView(
            ChromeTabbedActivity activity,
            ViewGroup coordinator,
            View bar,
            Spaces spaces,
            Callback<Integer> bottomHeight) {
        mActivity = activity;
        mCoordinator = coordinator;
        mSpaces = spaces;
        mBottomHeight = bottomHeight;
        mDensity = activity.getResources().getDisplayMetrics().density;
        mHost = new FrameLayout(activity);
        mHost.setVisibility(View.GONE);
        mOverlay = SplitUi.createView(activity, activity, activity, mState, this);
        // Under the bar, which floats over the bottom pane; the strips over the pages.
        int at = Math.max(0, coordinator.indexOfChild(bar));
        coordinator.addView(
                mOverlay,
                at,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        coordinator.addView(mHost, at, new CoordinatorLayout.LayoutParams(0, 0));
        coordinator.addOnLayoutChangeListener(
                (v, l, t, r, b, ol, ot, or, ob) -> {
                    if (r - l != or - ol || b - t != ob - ot) postUpdate();
                });
        mTopControls = activity.getRootUiCoordinatorForTesting().getTopControlsStacker();
        if (mTopControls != null) mTopControls.addControl(mTopLayer);
    }

    public boolean isShowing() {
        return mState.getMode() != SplitUi.Mode.NONE;
    }

    /** The shown tab, the spaces or the window's size changed: what the window shows follows. */
    public void postUpdate() {
        if (mUpdatePosted) return;
        mUpdatePosted = true;
        mHandler.post(
                () -> {
                    mUpdatePosted = false;
                    update();
                });
    }

    /** The space's accent, which outlines the front pane. */
    public void setAccent(int argb) {
        mState.setAccent(argb);
    }

    public void destroy() {
        clear();
        if (mTopControls != null) mTopControls.removeControl(mTopLayer);
        mHandler.removeCallbacksAndMessages(null);
    }

    private void update() {
        Tab front = mActivity.getActivityTab();
        SidebarState.Node split =
                front == null || front.isIncognito()
                        ? null
                        : mSpaces.state().splitOf(front.getId());
        if (split == null || split.children.size() < 2) {
            clear();
            return;
        }
        int width = mCoordinator.getWidth();
        int height = mCoordinator.getHeight();
        if (width == 0 || height == 0) return;
        int strip = Math.round(STRIP_DP * mDensity);
        List<SidebarState.Node> panes = split.children;
        int frontIndex = 0;
        for (int i = 0; i < panes.size(); i++) {
            if (panes.get(i).tab == front.getId()) frontIndex = i;
        }

        List<SplitUi.Pane> shown = new ArrayList<>();
        if (panes.size() > 2) {
            // One pane at a time, under the picker.
            release();
            int picker = Math.round(PICKER_DP * mDensity);
            for (int i = 0; i < panes.size(); i++) {
                SidebarState.Node pane = panes.get(i);
                shown.add(
                        new SplitUi.Pane(
                                pane.id, pane.title, pane.url, i == frontIndex, new Rect()));
            }
            mState.show(SplitUi.Mode.PICKER, shown, strip, picker);
            setInsets(picker, 0, 0, 0);
            return;
        }

        SidebarState.Node other = panes.get(1 - frontIndex);
        boolean sideBySide = width > height;
        Rect[] rects = new Rect[2];
        if (sideBySide) {
            rects[0] = new Rect(0, 0, width / 2, height);
            rects[1] = new Rect(width / 2, 0, width, height);
        } else {
            rects[0] = new Rect(0, 0, width, height / 2);
            rects[1] = new Rect(0, height / 2, width, height);
        }
        for (int i = 0; i < 2; i++) {
            SidebarState.Node pane = panes.get(i);
            shown.add(new SplitUi.Pane(pane.id, pane.title, pane.url, i == frontIndex, rects[i]));
        }
        mState.show(SplitUi.Mode.TOGETHER, shown, strip, 0);

        // The front pane's page is Chrome's: the rest of the window goes to its controls.
        if (sideBySide) {
            setInsets(
                    strip,
                    0,
                    frontIndex == 1 ? rects[0].width() : 0,
                    frontIndex == 0 ? rects[1].width() : 0);
        } else if (frontIndex == 0) {
            setInsets(strip, height - rects[1].top, 0, 0);
        } else {
            setInsets(rects[0].bottom + strip, 0, 0, 0);
        }

        // The other pane's page, below its strip; its tab opens in the background if it has none.
        Tab otherTab = other.tab >= 0 ? tabById(other.tab) : null;
        if (otherTab == null) {
            release();
            if (other.tab < 0 && mOpening.add(other.id)) {
                mSpaces.openInBackground(other.id, other.url);
            }
            return;
        }
        mOpening.remove(other.id);
        Rect page = rects[1 - frontIndex];
        CoordinatorLayout.LayoutParams params =
                (CoordinatorLayout.LayoutParams) mHost.getLayoutParams();
        int pageHeight = page.height() - strip;
        if (params.width != page.width()
                || params.height != pageHeight
                || params.leftMargin != page.left
                || params.topMargin != page.top + strip) {
            params.width = page.width();
            params.height = pageHeight;
            params.leftMargin = page.left;
            params.topMargin = page.top + strip;
            mHost.setLayoutParams(params);
        }
        borrow(otherTab);
    }

    private @Nullable Tab tabById(int id) {
        TabModel model = mActivity.getTabModelSelector().getModel(false);
        return model.getTabById(id);
    }

    // Top controls' height, CompassBar's bottom layer's, and the widths beside the page.
    private void setInsets(int top, int bottom, int left, int right) {
        if (mTopHeight != top) {
            mTopHeight = top;
            if (mTopControls != null) mTopControls.requestLayerUpdateSync(false);
        }
        mBottomHeight.onResult(bottom);
        CompositorViewHolder holder = mActivity.getCompositorViewHolderSupplier().get();
        if (holder != null) holder.setCompassSideInsets(left, right);
    }

    private void clear() {
        mState.hide();
        release();
        mOpening.clear();
        setInsets(0, 0, 0, 0);
    }

    private void borrow(Tab tab) {
        if (mBorrowed == tab && mThinWebView != null) return;
        release();
        // A tab restored frozen gets its page now.
        if (tab.getWebContents() == null) tab.loadIfNeeded(/* forceBackingSize= */ false);
        WebContents contents = tab.getWebContents();
        ContentView contentView = tab.getContentView();
        if (contents == null || contentView == null || tab == mActivity.getActivityTab()) return;
        // Still the window's (it was just in front): once Chrome lets it go.
        if (contentView.getParent() != null) {
            mHandler.postDelayed(this::postUpdate, 50);
            return;
        }
        ThinWebViewConstraints constraints = new ThinWebViewConstraints();
        constraints.supportsOpacity = true;
        constraints.backgroundColor = Color.WHITE;
        ThinWebView view =
                ThinWebViewFactory.create(
                        mActivity,
                        constraints,
                        mActivity.getWindowAndroid().getIntentRequestTracker(),
                        /* enablePermissionRequests= */ true);
        // No delegate: the page keeps its tab's.
        view.attachWebContents(contents, contentView, new ThinWebViewAttachParams.Builder().build());
        mHost.addView(
                view.getView(),
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        mHost.setVisibility(View.VISIBLE);
        mThinWebView = view;
        mBorrowed = tab;
        tab.addObserver(mBorrowedObserver);
    }

    /** The borrowed page goes back to the window, hidden unless it's the shown tab. */
    private void release() {
        Tab tab = mBorrowed;
        ThinWebView view = mThinWebView;
        mBorrowed = null;
        mThinWebView = null;
        mHost.removeAllViews();
        mHost.setVisibility(View.GONE);
        if (tab == null || view == null) return;
        tab.removeObserver(mBorrowedObserver);
        WebContents contents = tab.getWebContents();
        boolean live = contents != null && !contents.isDestroyed();
        // Back to the window before the ThinWebView's own window goes.
        if (live) contents.setTopLevelNativeWindow(mActivity.getWindowAndroid());
        view.destroy();
        // Its layer, which the ThinWebView's compositor took, goes back where Chrome draws it.
        if (live) SplitBridge.returnLayerToTab(contents);
        if (live && mActivity.getActivityTab() != tab && !tab.isClosing()) {
            contents.updateWebContentsVisibility(Visibility.HIDDEN);
        }
    }

    // SplitUi.Listener:

    @Override
    public void onFront(String paneId) {
        SidebarState.Node pane = mSpaces.state().findNode(paneId);
        if (pane == null) return;
        Tab tab = pane.tab >= 0 ? tabById(pane.tab) : null;
        if (tab == null) {
            mSpaces.onNodeClicked(pane.id, pane.url);
            return;
        }
        // It goes back to the window before Chrome shows it.
        if (tab == mBorrowed) release();
        TabModelUtils.selectTabById(
                mActivity.getTabModelSelector(), tab.getId(), TabSelectionType.FROM_USER);
    }
}
