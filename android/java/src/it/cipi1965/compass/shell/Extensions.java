// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.toolbar.extensions.ExtensionActionPopup;
import org.chromium.chrome.browser.ui.browser_window.ChromeAndroidTask;
import org.chromium.chrome.browser.ui.browser_window.ChromeAndroidTaskTrackerFactory;
import org.chromium.chrome.browser.ui.extensions.ExtensionAction;
import org.chromium.chrome.browser.ui.extensions.ExtensionActionPopupContents;
import org.chromium.chrome.browser.ui.extensions.ExtensionsToolbarBridge;
import org.chromium.chrome.browser.ui.toolbar.InvocationSource;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.base.PageTransition;
import org.chromium.ui.base.WindowAndroid;

import java.util.ArrayList;
import java.util.List;

/**
 * Extensions in the shell (docs/design/android.md, "Extensions"): the sheet (ExtensionsUi.kt)
 * over the window's extension actions, through upstream's ExtensionsToolbarBridge. Chrome's own
 * extensions toolbar is part of its top toolbar, which Compass replaces; an action's popup is
 * Chrome's ExtensionActionPopup, anchored to Compass's bar.
 */
public class Extensions
        implements ExtensionsUi.Listener,
                ExtensionsToolbarBridge.Observer,
                ExtensionsToolbarBridge.ActionListDelegate {
    private static final int ICON_DP = 28;
    private static final String WEB_STORE_URL = "https://chromewebstore.google.com/";

    private final ChromeTabbedActivity mActivity;
    private final View mAnchor;
    private final ExtensionsState mState = new ExtensionsState();
    private final View mView;
    private @Nullable ExtensionsToolbarBridge mBridge;
    private @Nullable ExtensionActionPopup mPopup;

    public Extensions(ChromeTabbedActivity activity, ViewGroup parent, View anchor) {
        mActivity = activity;
        mAnchor = anchor;
        mView = ExtensionsUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        ChromeAndroidTask task =
                ChromeAndroidTaskTrackerFactory.getInstance().get(activity.getTaskId());
        if (task == null) {
            mState.setAvailable(false);
            return;
        }
        mBridge =
                new ExtensionsToolbarBridge(
                        task, activity.getTabModelSelector().getModel(false).getProfile());
        mBridge.addObserver(this);
        mBridge.setActionListDelegate(this);
    }

    public void destroy() {
        closePopup();
        if (mBridge != null) {
            mBridge.setActionListDelegate(null);
            mBridge.removeObserver(this);
            mBridge.destroy();
            mBridge = null;
        }
    }

    public void show() {
        refresh();
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
    }

    /** Extensions' keyboard shortcuts (their commands). */
    public boolean dispatchKeyEvent(KeyEvent event) {
        return mBridge != null
                && event.getAction() == KeyEvent.ACTION_DOWN
                && mBridge.handleKeyDownEvent(event);
    }

    private void refresh() {
        if (mBridge == null) return;
        Tab tab = mActivity.getActivityTab();
        @Nullable WebContents webContents = tab != null ? tab.getWebContents() : null;
        float scale = mActivity.getResources().getDisplayMetrics().density;
        List<ExtensionsState.Action> actions = new ArrayList<>();
        for (String id : mBridge.getAllActionIds()) {
            ExtensionAction action = mBridge.getAction(id, webContents);
            if (action == null) continue;
            actions.add(
                    new ExtensionsState.Action(
                            id,
                            action.getName(),
                            action.getTitle(),
                            mBridge.getIcon(id, webContents, ICON_DP, ICON_DP, scale)));
        }
        mState.setActions(actions);
    }

    private void openInNewTab(String url) {
        onDismiss();
        mActivity
                .getTabCreator(false)
                .createNewTab(
                        new LoadUrlParams(url, PageTransition.AUTO_TOPLEVEL),
                        TabLaunchType.FROM_CHROME_UI,
                        mActivity.getActivityTab());
    }

    private void closePopup() {
        ExtensionActionPopup popup = mPopup;
        // Cleared first: destroying it calls the dismiss listener.
        mPopup = null;
        if (popup != null) popup.destroy();
    }

    // ExtensionsUi.Listener:

    @Override
    public void onRunAction(String id) {
        onDismiss();
        if (mBridge != null) mBridge.executeUserAction(id, InvocationSource.MENU_ENTRY);
    }

    @Override
    public void onManage() {
        openInNewTab("chrome://extensions/");
    }

    @Override
    public void onWebStore() {
        openInNewTab(WEB_STORE_URL);
    }

    @Override
    public void onDismiss() {
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }

    // ExtensionsToolbarBridge.Observer:

    @Override
    public void onActionsInitialized() {
        if (mState.getOpen()) refresh();
    }

    @Override
    public void onActionAdded(String actionId) {
        if (mState.getOpen()) refresh();
    }

    @Override
    public void onActionRemoved(String actionId) {
        if (mState.getOpen()) refresh();
    }

    @Override
    public void onActionUpdated(String actionId) {
        if (mState.getOpen()) refresh();
    }

    // ExtensionsToolbarBridge.ActionListDelegate:

    @Override
    public void triggerPopup(
            String actionId,
            ExtensionActionPopupContents popupContents,
            boolean inspectWithDevTools) {
        closePopup();
        WindowAndroid window = mActivity.getWindowAndroid();
        if (window == null) {
            popupContents.destroy();
            return;
        }
        ExtensionActionPopup popup =
                new ExtensionActionPopup(
                        mActivity,
                        window,
                        mAnchor,
                        actionId,
                        popupContents,
                        /* contextMenuPopulatorFactory= */ null,
                        /* selectionDropdownMenuDelegate= */ null,
                        mActivity.getTabModelSelector(),
                        inspectWithDevTools);
        mPopup = popup;
        popup.loadInitialPage();
        popup.addOnDismissListener(this::closePopup);
    }

    @Override
    public void showContextMenu(String actionId) {}

    @Override
    public boolean hasPoppedOutAction() {
        return false;
    }

    @Override
    public void hideActivePopup() {
        closePopup();
    }

    @Override
    public boolean hasActivePopup() {
        return mPopup != null;
    }
}
