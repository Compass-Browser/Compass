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
    private long mNativeSyncBridge;
    private final Runnable mOnChanged;

    public SyncBridge(Profile profile, Runnable onChanged) {
        mOnChanged = onChanged;
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

    public void restart() {
        SyncBridgeJni.get().restart(mNativeSyncBridge);
    }

    public void setDataTypeEnabled(int type, boolean enabled) {
        SyncBridgeJni.get().setDataTypeEnabled(mNativeSyncBridge, type, enabled);
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

        void restart(long nativeSyncBridge);

        void setDataTypeEnabled(long nativeSyncBridge, int type, boolean enabled);
    }
}
