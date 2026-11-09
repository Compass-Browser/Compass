// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.view.KeyEvent;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.extras.CompassIntents;

import org.chromium.base.Callback;
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
import org.chromium.chrome.browser.lifecycle.DestroyObserver;
import org.chromium.chrome.browser.omnibox.LocationBarCoordinator;
import org.chromium.chrome.browser.preferences.Pref;
import org.chromium.chrome.browser.toolbar.ToolbarManager;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.ui.appmenu.AppMenuHandler;
import org.chromium.components.user_prefs.UserPrefs;
import org.chromium.url.GURL;

import java.util.Map;
import java.util.WeakHashMap;

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
    private @Nullable Spaces mSpaces;
    private @Nullable ClaudeChat mClaude;
    private @Nullable Integer mBackgroundColor;
    private @Nullable SyncScreen mSync;
    private @Nullable Extensions mExtensions;
    private ActivityTabProvider.@Nullable ActivityTabTabObserver mTabObserver;
    private @Nullable TabModel mTabs;
    private final Callback<Integer> mTabCountObserver =
            count -> {
                if (mTabs != null) mState.setTabCount(mTabs.getCount());
            };

    // Each activity's bar, for the pages that need it (the start page).
    private static final Map<Activity, CompassBar> sBars = new WeakHashMap<>();

    /** The bar of |activity|, if it has one. */
    public static @Nullable CompassBar forActivity(Activity activity) {
        return sBars.get(activity);
    }

    public CompassBar(ChromeTabbedActivity activity, BottomControlsStacker bottomControls) {
        mActivity = activity;
        mBottomControls = bottomControls;
        mHeightPx = Math.round(HEIGHT_DP * activity.getResources().getDisplayMetrics().density);
    }

    public void attach() {
        sBars.put(mActivity, this);
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
        Spaces spaces = new Spaces(mActivity, coordinator);
        mSpaces = spaces;
        SyncScreen sync = new SyncScreen(mActivity, coordinator);
        mSync = sync;
        mClaude = new ClaudeChat(mActivity, coordinator);
        mExtensions = new Extensions(mActivity, coordinator, view);
        spaces.setOnOpenSync(sync::show);
        // Compass has no browser sign-in (as on the desktop): no sign-in promos either.
        UserPrefs.get(mActivity.getTabModelSelector().getModel(false).getProfile())
                .setBoolean(Pref.SIGNIN_ALLOWED, false);
        redirectOmniboxFocus();
        // Compass's own intents (the tile, the widget, shortcuts, the share target).
        handleIntent(mActivity.getIntent());
        spaces.setOnChanged(
                () -> {
                    SidebarState.Space space = spaces.state().getActiveSpace();
                    mState.setSpaceLabel(
                            space == null
                                    ? ""
                                    : !space.emoji.isEmpty()
                                            ? space.emoji
                                            : space.name.isEmpty()
                                                    ? ""
                                                    : space.name.substring(0, 1).toUpperCase());
                    mState.setSpaceTint(space == null ? "" : space.tint);
                });

        // Registers itself with the tab provider.
        mTabObserver =
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
        mTabs = tabs;
        tabs.getTabCountSupplier().addSyncObserverAndCall(mTabCountObserver);

        mBottomControls.addLayer(this);
        mBottomControls.requestLayerUpdate(false);
        mActivity.getLifecycleDispatcher().register((DestroyObserver) this::destroy);
    }

    // The activity is going away (a window closed, or Android recreating it): the native bridges
    // and the tab observers go with it.
    private void destroy() {
        sBars.remove(mActivity);
        if (mTabObserver != null) mTabObserver.destroy();
        if (mTabs != null) mTabs.getTabCountSupplier().removeObserver(mTabCountObserver);
        mBottomControls.removeLayer(this);
        if (mSpaces != null) mSpaces.destroy();
        if (mSync != null) mSync.destroy();
        if (mClaude != null) mClaude.destroy();
        if (mExtensions != null) mExtensions.destroy();
        mExtensions = null;
        mTabObserver = null;
        mSpaces = null;
        mSync = null;
        mClaude = null;
        mCommandBar = null;
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

    // Chrome still focuses its (hidden) location bar: from the new tab page's search box, or
    // Ctrl+L. The command bar takes over.
    private void redirectOmniboxFocus() {
        ToolbarManager toolbar = mActivity.getToolbarManager();
        if (toolbar == null) return;
        toolbar.getOmniboxStubSupplier()
                .onAvailable(
                        omnibox ->
                                omnibox.addUrlFocusChangeListener(
                                        hasFocus -> {
                                            if (!hasFocus || mCommandBar == null) return;
                                            if (toolbar.getLocationBar()
                                                    instanceof LocationBarCoordinator location) {
                                                location.clearOmniboxFocus();
                                            }
                                            // For the tab on screen (on the new tab page,
                                            // that's the new tab).
                                            mCommandBar.open(/* inNewTab= */ false);
                                        }));
    }

    /** Handles Compass's own intents (CompassIntents); returns whether |intent| was one. */
    public boolean handleIntent(@Nullable Intent intent) {
        if (intent == null || !CompassIntents.isCompassIntent(intent)) return false;
        String action = intent.getAction();
        if (CompassIntents.ACTION_COMMAND_BAR.equals(action)) {
            if (mCommandBar != null) mCommandBar.open(/* inNewTab= */ true);
        } else if (CompassIntents.ACTION_OPEN_SPACE.equals(action)) {
            String spaceId = intent.getStringExtra(CompassIntents.EXTRA_SPACE_ID);
            if (spaceId != null && mSpaces != null) mSpaces.openSpace(spaceId);
        } else if (CompassIntents.ACTION_PIN_URL.equals(action)) {
            String url = intent.getStringExtra(CompassIntents.EXTRA_URL);
            if (url != null && mSpaces != null) mSpaces.openAndPin(url);
        }
        // Handled once, even if the activity is recreated with it.
        intent.setAction(null);
        return true;
    }

    /**
     * Compass's keyboard shortcuts, as on the desktop (Ctrl for Cmd); returns whether |event| was
     * one. The rest are Chrome's (KeyboardShortcuts).
     */
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (mExtensions != null && mExtensions.dispatchKeyEvent(event)) return true;
        if (!event.isCtrlPressed() || event.isAltPressed() || event.isMetaPressed()) return false;
        boolean shift = event.isShiftPressed();
        int key = event.getKeyCode();
        boolean ours =
                shift
                        ? key == KeyEvent.KEYCODE_E || key == KeyEvent.KEYCODE_C
                        : key == KeyEvent.KEYCODE_S
                                || key == KeyEvent.KEYCODE_T
                                || key == KeyEvent.KEYCODE_L
                                || key == KeyEvent.KEYCODE_K;
        if (!ours) return false;
        // Both halves of the key press are ours; the action runs on the first.
        if (event.getAction() != KeyEvent.ACTION_DOWN || event.getRepeatCount() != 0) return true;
        if (shift && key == KeyEvent.KEYCODE_E) {
            showClaude();
        } else if (shift) {
            copyUrl();
        } else if (key == KeyEvent.KEYCODE_S) {
            if (mSpaces != null) mSpaces.toggleSidebar();
        } else if (key == KeyEvent.KEYCODE_T) {
            newTab();
        } else {
            openAddress();
        }
        return true;
    }

    /** Shows the extensions sheet; false if the window has none. */
    public boolean showExtensions() {
        if (mExtensions == null) return false;
        mExtensions.show();
        return true;
    }

    /** Opens a sidebar entry: its tab, or a new one for it. */
    public void openNode(String nodeId, String url) {
        if (mSpaces != null) mSpaces.onNodeClicked(nodeId, url);
    }

    private void copyUrl() {
        Tab tab = mActivity.getActivityTab();
        if (tab == null) return;
        ClipboardManager clipboard =
                (ClipboardManager) mActivity.getSystemService(Context.CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(ClipData.newPlainText("URL", tab.getUrl().getSpec()));
    }

    // CompassBarUi.Actions:

    @Override
    public void showSidebar() {
        if (mSpaces != null) mSpaces.showSidebar();
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
    public void showClaude() {
        if (mClaude != null) mClaude.show();
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
