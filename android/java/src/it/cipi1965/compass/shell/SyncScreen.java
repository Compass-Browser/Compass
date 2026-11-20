// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.sync.SyncBridge;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.tabmodel.TabClosureParams;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.ui.base.PageTransition;
import org.chromium.url.GURL;

/** The sync screen (SyncScreenUi.kt) over the profile's Compass sync (SyncBridge). */
public class SyncScreen implements SyncScreenUi.Listener, SyncBridge.LoginPages {
    private final ChromeTabbedActivity mActivity;
    private final SyncBridge mBridge;
    private final SyncScreenState mState = new SyncScreenState();
    private final View mView;
    // A company sign-in's page (docs/design/sync.md, "Company sign-in (OIDC)"), while it runs.
    private @Nullable Tab mLoginTab;
    private @Nullable GURL mLoginUrl;

    public SyncScreen(ChromeTabbedActivity activity, ViewGroup parent) {
        mActivity = activity;
        mBridge =
                new SyncBridge(
                        activity.getTabModelSelector().getModel(false).getProfile(),
                        this::refresh,
                        this);
        mView = SyncScreenUi.createView(activity, activity, activity, activity, mState, this);
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

    /**
     * A pairing invite (a desktop's QR code): joins with it, unless this device is in a chain
     * already, and shows how it goes.
     */
    public void join(String server, String code) {
        show();
        if ("off".equals(mState.getState())) onJoinWithPairingCode(server, code);
    }

    /** A line for Settings' Sync row: off, or how it goes. */
    public String summary() {
        refresh();
        return "off".equals(mState.getState()) ? "Off" : mState.getMessage();
    }

    private void refresh() {
        mState.update(mBridge.snapshot());
    }

    @Override
    public void onJoinWithPairingCode(String server, String code) {
        mBridge.joinWithPairingCode(server, code);
    }

    @Override
    public void onJoinWithRecoveryCode(String server, String words) {
        mBridge.joinWithRecoveryCode(server, words);
    }

    @Override
    public void onStartNewChain(String server) {
        mBridge.startNewChain(server);
    }

    @Override
    public void onAddDevice() {
        mBridge.addDevice();
    }

    @Override
    public void onAnswerPairing(boolean allow) {
        mBridge.answerPairing(allow);
    }

    @Override
    public void onCancelPairing() {
        mBridge.cancelPairing();
    }

    @Override
    public void onLeave() {
        mBridge.leave();
    }

    @Override
    public void onRestart() {
        mBridge.restart();
    }

    @Override
    public void onSetDataType(int type, boolean enabled) {
        mBridge.setDataTypeEnabled(type, enabled);
    }

    @Override
    public void onStartLogin() {
        mBridge.startLogin();
    }

    @Override
    public void onCancelLogin() {
        mBridge.cancelLogin();
    }

    /** SyncBridge.LoginPages: the server's page in a new tab, over this screen. */
    @Override
    public void open(String url) {
        mLoginUrl = new GURL(url);
        mLoginTab =
                mActivity
                        .getTabCreator(false)
                        .createNewTab(
                                new LoadUrlParams(url, PageTransition.LINK),
                                TabLaunchType.FROM_CHROME_UI,
                                mActivity.getActivityTab());
        onDismiss();
    }

    /**
     * SyncBridge.LoginPages: the sign-in ended. Its tab closes if it still shows the server's
     * pages ("You can return to Compass"), and this screen shows how it went.
     */
    @Override
    public void close() {
        Tab tab = mLoginTab;
        GURL opened = mLoginUrl;
        mLoginTab = null;
        mLoginUrl = null;
        if (tab == null || opened == null) return;
        boolean onServer =
                !tab.isClosing()
                        && !tab.isDestroyed()
                        && tab.getUrl().getScheme().equals(opened.getScheme())
                        && tab.getUrl().getHost().equals(opened.getHost())
                        && tab.getUrl().getPort().equals(opened.getPort());
        if (!onServer) return;
        mActivity
                .getTabModelSelector()
                .getModel(tab.isIncognito())
                .getTabRemover()
                .closeTabs(
                        TabClosureParams.closeTab(tab).allowUndo(false).build(),
                        /* allowDialog= */ false);
        show();
    }

    @Override
    public void onDismiss() {
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
