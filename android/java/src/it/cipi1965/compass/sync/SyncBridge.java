// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.sync;

import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.chrome.browser.profiles.Profile;

/**
 * Java side of the profile's Compass sync (//compass/browser/android: SyncBridge, over the
 * desktop's CompassSyncService): status and pairing as JSON snapshots, actions as calls.
 */
@JNINamespace("compass")
public class SyncBridge {
    /**
     * Where a company sign-in's page shows (docs/design/sync.md, "Company sign-in (OIDC)"): the
     * screen that started the sign-in opens it in a tab, and closes it when the sign-in ends.
     */
    public interface LoginPages {
        void open(String url);

        void close();

        /**
         * "Sign in with Cloudflare" (docs/design/sync.md, "A free server on Cloudflare"): the
         * service opened Cloudflare's sign-in in a Compass tab ({@code open} true) and catches its
         * redirect there; the screen steps aside until the tab closes ({@code open} false).
         */
        void cloudflareSignIn(boolean open);
    }

    private long mNativeSyncBridge;
    private final Runnable mOnChanged;
    private final LoginPages mLoginPages;

    public SyncBridge(Profile profile, Runnable onChanged, LoginPages loginPages) {
        mOnChanged = onChanged;
        mLoginPages = loginPages;
        mNativeSyncBridge = SyncBridgeJni.get().init(this, profile);
    }

    public void destroy() {
        if (mNativeSyncBridge == 0) return;
        SyncBridgeJni.get().destroy(mNativeSyncBridge);
        mNativeSyncBridge = 0;
    }

    @CalledByNative
    private void onChanged() {
        mOnChanged.run();
    }

    @CalledByNative
    private void openLoginPage(@JniType("std::string") String url) {
        mLoginPages.open(url);
    }

    @CalledByNative
    private void closeLoginPage() {
        mLoginPages.close();
    }

    @CalledByNative
    private void cloudflareSignIn(boolean open) {
        mLoginPages.cloudflareSignIn(open);
    }

    public String snapshot() {
        return SyncBridgeJni.get().snapshot(mNativeSyncBridge);
    }

    public void startNewChain(String serverUrl) {
        SyncBridgeJni.get().startNewChain(mNativeSyncBridge, serverUrl);
    }

    public void joinWithPairingCode(String serverUrl, String code) {
        SyncBridgeJni.get().joinWithPairingCode(mNativeSyncBridge, serverUrl, code);
    }

    public void joinWithRecoveryCode(String serverUrl, String words) {
        SyncBridgeJni.get().joinWithRecoveryCode(mNativeSyncBridge, serverUrl, words);
    }

    public void addDevice() {
        SyncBridgeJni.get().addDevice(mNativeSyncBridge);
    }

    public void answerPairing(boolean allow) {
        SyncBridgeJni.get().answerPairing(mNativeSyncBridge, allow);
    }

    public void cancelPairing() {
        SyncBridgeJni.get().cancelPairing(mNativeSyncBridge);
    }

    public void leave() {
        SyncBridgeJni.get().leave(mNativeSyncBridge);
    }

    /** Removes another device of the chain (its cache GUID, from the snapshot). */
    public void removeDevice(String guid) {
        SyncBridgeJni.get().removeDevice(mNativeSyncBridge, guid);
    }

    public void restart() {
        SyncBridgeJni.get().restart(mNativeSyncBridge);
    }

    public void startLogin() {
        SyncBridgeJni.get().startLogin(mNativeSyncBridge);
    }

    public void cancelLogin() {
        SyncBridgeJni.get().cancelLogin(mNativeSyncBridge);
    }

    public void setDataTypeEnabled(int type, boolean enabled) {
        SyncBridgeJni.get().setDataTypeEnabled(mNativeSyncBridge, type, enabled);
    }

    /**
     * The chain's 12 words, or none if sync isn't running. Only after the screen lock (as the
     * desktop's "Show recovery code"); never log them.
     */
    public String[] getRecoveryCode() {
        if (mNativeSyncBridge == 0) return new String[0];
        return SyncBridgeJni.get().getRecoveryCode(mNativeSyncBridge);
    }

    /** "I saved it": forgets a new chain's recovery code, shown once. */
    public void clearNewWords() {
        if (mNativeSyncBridge == 0) return;
        SyncBridgeJni.get().clearNewWords(mNativeSyncBridge);
    }

    /** Sets up a free server on Cloudflare with an API token (the snapshot shows how it goes). */
    public void startOnCloudflare(String token, boolean pushRelay) {
        SyncBridgeJni.get().startOnCloudflare(mNativeSyncBridge, token, pushRelay);
    }

    /** As {@link #startOnCloudflare}, after "Sign in with Cloudflare" in a tab. */
    public void startOnCloudflareSignIn(boolean pushRelay) {
        SyncBridgeJni.get().startOnCloudflareSignIn(mNativeSyncBridge, pushRelay);
    }

    /** "I have an invite": https://<server>/invite#<code>. */
    public void joinWithInvite(String link) {
        SyncBridgeJni.get().joinWithInvite(mNativeSyncBridge, link);
    }

    /** Asks the server again for its people and an update (the owner's). */
    public void loadPeople() {
        if (mNativeSyncBridge == 0) return;
        SyncBridgeJni.get().loadPeople(mNativeSyncBridge);
    }

    public void inviteFriend(String name) {
        SyncBridgeJni.get().inviteFriend(mNativeSyncBridge, name);
    }

    /** Removes a friend (their chain) or revokes an invite (its code). */
    public void removeFromServer(String target) {
        SyncBridgeJni.get().removeFromServer(mNativeSyncBridge, target);
    }

    public void setServerPushRelay(boolean on) {
        SyncBridgeJni.get().setServerPushRelay(mNativeSyncBridge, on);
    }

    /** Updates the server: with an API token, or (empty) after signing in with Cloudflare. */
    public void updateServer(String token) {
        SyncBridgeJni.get().updateServer(mNativeSyncBridge, token);
    }

    @NativeMethods
    interface Natives {
        long init(SyncBridge caller, @JniType("Profile*") Profile profile);

        void destroy(long nativeSyncBridge);

        @JniType("std::string")
        String snapshot(long nativeSyncBridge);

        void startNewChain(long nativeSyncBridge, @JniType("std::string") String serverUrl);

        void joinWithPairingCode(
                long nativeSyncBridge,
                @JniType("std::string") String serverUrl,
                @JniType("std::string") String code);

        void joinWithRecoveryCode(
                long nativeSyncBridge,
                @JniType("std::string") String serverUrl,
                @JniType("std::string") String words);

        void addDevice(long nativeSyncBridge);

        void answerPairing(long nativeSyncBridge, boolean allow);

        void cancelPairing(long nativeSyncBridge);

        void leave(long nativeSyncBridge);

        void removeDevice(long nativeSyncBridge, @JniType("std::string") String guid);

        void restart(long nativeSyncBridge);

        void startLogin(long nativeSyncBridge);

        void cancelLogin(long nativeSyncBridge);

        void setDataTypeEnabled(long nativeSyncBridge, int type, boolean enabled);

        @JniType("std::vector<std::string>")
        String[] getRecoveryCode(long nativeSyncBridge);

        void clearNewWords(long nativeSyncBridge);

        void startOnCloudflare(
                long nativeSyncBridge, @JniType("std::string") String token, boolean pushRelay);

        void startOnCloudflareSignIn(long nativeSyncBridge, boolean pushRelay);

        void joinWithInvite(long nativeSyncBridge, @JniType("std::string") String link);

        void loadPeople(long nativeSyncBridge);

        void inviteFriend(long nativeSyncBridge, @JniType("std::string") String name);

        void removeFromServer(long nativeSyncBridge, @JniType("std::string") String target);

        void setServerPushRelay(long nativeSyncBridge, boolean on);

        void updateServer(long nativeSyncBridge, @JniType("std::string") String token);
    }
}
