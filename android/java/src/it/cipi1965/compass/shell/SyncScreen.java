// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.sync.SyncBridge;

import org.chromium.chrome.browser.ChromeTabbedActivity;

/** The sync screen (SyncScreenUi.kt) over the profile's Compass sync (SyncBridge). */
public class SyncScreen implements SyncScreenUi.Listener {
    private final SyncBridge mBridge;
    private final SyncScreenState mState = new SyncScreenState();
    private final View mView;

    public SyncScreen(ChromeTabbedActivity activity, ViewGroup parent) {
        mBridge =
                new SyncBridge(
                        activity.getTabModelSelector().getModel(false).getProfile(),
                        this::refresh);
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
    public void onDismiss() {
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
