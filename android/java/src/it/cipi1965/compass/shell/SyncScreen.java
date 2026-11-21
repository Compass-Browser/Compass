// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.app.KeyguardManager;
import android.hardware.biometrics.BiometricManager;
import android.hardware.biometrics.BiometricPrompt;
import android.os.Build;
import android.os.CancellationSignal;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;

import androidx.annotation.NonNull;
import androidx.coordinatorlayout.widget.CoordinatorLayout;
import androidx.lifecycle.DefaultLifecycleObserver;
import androidx.lifecycle.LifecycleOwner;

import it.cipi1965.compass.sync.SyncBridge;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.tabmodel.TabClosureParams;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.ui.base.PageTransition;
import org.chromium.url.GURL;

import java.util.Arrays;

/** The sync screen (SyncScreenUi.kt) over the profile's Compass sync (SyncBridge). */
public class SyncScreen implements SyncScreenUi.Listener, SyncBridge.LoginPages {
    private final ChromeTabbedActivity mActivity;
    private final SyncBridge mBridge;
    private final SyncScreenState mState = new SyncScreenState();
    private final View mView;
    // The screen lock's prompt for "Show recovery code", while it's up.
    private CancellationSignal mUnlock;
    // While recovery words show: FLAG_SECURE is ours to clear (the incognito snapshot
    // controller sets it too, so it isn't cleared if it was set before).
    private boolean mAddedSecureFlag;
    private final DefaultLifecycleObserver mLifecycleObserver =
            new DefaultLifecycleObserver() {
                @Override
                public void onStop(@NonNull LifecycleOwner owner) {
                    // Leaving the app hides the words, as leaving the screen does. The
                    // prompt stays: on Android 10 its PIN screen is an activity of its own,
                    // and the system cancels a prompt whose app goes away.
                    clearRecoveryCode();
                }
            };
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
        activity.getLifecycle().addObserver(mLifecycleObserver);
    }

    public void destroy() {
        hideRecoveryCode();
        mActivity.getLifecycle().removeObserver(mLifecycleObserver);
        mBridge.destroy();
    }

    public void show() {
        refresh();
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
        updateSecureFlag();
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
        // Sync stopped (left, reset): the words shown are no longer this device's.
        String state = mState.getState();
        if (!"on".equals(state) && !"starting".equals(state) && !"error".equals(state)) {
            hideRecoveryCode();
        }
        updateSecureFlag();
    }

    /**
     * FLAG_SECURE while words show: a new chain's (until "I saved it") or the revealed ones. No
     * screenshots or screen recording, and a blank thumbnail in Recents.
     */
    private void updateSecureFlag() {
        boolean wordsShown =
                mState.getOpen()
                        && (!mState.getRecoveryWords().isEmpty()
                                || !mState.getNewWords().isEmpty());
        Window window = mActivity.getWindow();
        if (wordsShown && !mAddedSecureFlag) {
            if ((window.getAttributes().flags & WindowManager.LayoutParams.FLAG_SECURE) == 0) {
                window.addFlags(WindowManager.LayoutParams.FLAG_SECURE);
                mAddedSecureFlag = true;
            }
        } else if (!wordsShown && mAddedSecureFlag) {
            window.clearFlags(WindowManager.LayoutParams.FLAG_SECURE);
            mAddedSecureFlag = false;
        }
    }

    /**
     * "I saved it" under a new chain's words (shown once, without the screen lock, as the
     * desktop's "Start sync"): forgets them, then the screen goes on (Restart).
     */
    @Override
    public void onNewWordsSaved() {
        mBridge.clearNewWords();
        refresh();
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
    public void onRemoveDevice(String guid) {
        mBridge.removeDevice(guid);
    }

    @Override
    public void onRestart() {
        mBridge.restart();
    }

    @Override
    public void onSetDataType(int type, boolean enabled) {
        mBridge.setDataTypeEnabled(type, enabled);
    }

    /**
     * "Show recovery code": the screen lock first (fingerprint, face or the PIN, pattern or
     * password), as the desktop asks device_reauth. With no screen lock it doesn't ask, as the
     * desktop and Chrome don't, and says so next to the words.
     */
    @Override
    public void onShowRecoveryCode() {
        if (mUnlock != null) return;
        mState.setRecoveryError("");
        KeyguardManager keyguard = mActivity.getSystemService(KeyguardManager.class);
        if (keyguard == null || !keyguard.isDeviceSecure()) {
            revealRecoveryCode(/* unprotected= */ true);
            return;
        }
        BiometricPrompt.Builder builder =
                new BiometricPrompt.Builder(mActivity)
                        .setTitle("Show your sync recovery code");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            builder.setAllowedAuthenticators(
                    BiometricManager.Authenticators.BIOMETRIC_STRONG
                            | BiometricManager.Authenticators.DEVICE_CREDENTIAL);
        } else {
            // Android 10 (minSdk 29): setAllowedAuthenticators() comes in 11.
            builder.setDeviceCredentialAllowed(true);
        }
        CancellationSignal unlock = new CancellationSignal();
        mUnlock = unlock;
        builder.build()
                .authenticate(
                        unlock,
                        mActivity.getMainExecutor(),
                        new BiometricPrompt.AuthenticationCallback() {
                            @Override
                            public void onAuthenticationSucceeded(
                                    BiometricPrompt.AuthenticationResult result) {
                                if (mUnlock != unlock) return;
                                mUnlock = null;
                                // Not if the screen was left meanwhile.
                                if (mState.getOpen()) revealRecoveryCode(false);
                            }

                            @Override
                            public void onAuthenticationError(
                                    int code, CharSequence message) {
                                if (mUnlock != unlock) return;
                                mUnlock = null;
                                // Canceled: nothing to say, as the desktop.
                                if (code != BiometricPrompt.BIOMETRIC_ERROR_USER_CANCELED
                                        && code != BiometricPrompt.BIOMETRIC_ERROR_CANCELED) {
                                    mState.setRecoveryError(message.toString());
                                }
                            }
                        });
    }

    @Override
    public void onHideRecoveryCode() {
        hideRecoveryCode();
    }

    private void revealRecoveryCode(boolean unprotected) {
        // Read now, not before: the chain may have changed meanwhile.
        String[] words = mBridge.getRecoveryCode();
        if (words == null || words.length == 0) {
            mState.setRecoveryError("Sync isn't running on this device.");
            return;
        }
        mState.setRecoveryUnprotected(unprotected);
        mState.setRecoveryWords(Arrays.asList(words));
        updateSecureFlag();
    }

    private void hideRecoveryCode() {
        if (mUnlock != null) {
            CancellationSignal unlock = mUnlock;
            mUnlock = null;
            unlock.cancel();
        }
        clearRecoveryCode();
    }

    private void clearRecoveryCode() {
        mState.clearRecoveryCode();
        updateSecureFlag();
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
        hideRecoveryCode();
        mState.setOpen(false);
        updateSecureFlag();
        mView.setVisibility(View.GONE);
    }
}
