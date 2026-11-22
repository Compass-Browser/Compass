// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.build.annotations.NullMarked;

/**
 * The ad blocker (adblock_bridge.cc; docs/design/ad-blocker.md): its switch, for the whole
 * browser, and its filter lists. A site's own switch is its Ads site setting
 * (ContentSettingsType.ADS: allowed turns the blocker off there).
 */
@NullMarked
@JNINamespace("compass")
final class AdBlockBridge {
    private long mNativeAdBlockBridge;
    private final Runnable mOnChanged;

    /** Follows the lists: |onChanged| runs when they, their downloads or the switch change. */
    AdBlockBridge(Runnable onChanged) {
        mOnChanged = onChanged;
        mNativeAdBlockBridge = AdBlockBridgeJni.get().init(this);
    }

    void destroy() {
        if (mNativeAdBlockBridge == 0) return;
        AdBlockBridgeJni.get().destroy(mNativeAdBlockBridge);
        mNativeAdBlockBridge = 0;
    }

    @CalledByNative
    private void onChanged() {
        mOnChanged.run();
    }

    static boolean isEnabled() {
        return AdBlockBridgeJni.get().isEnabled();
    }

    static void setEnabled(boolean enabled) {
        AdBlockBridgeJni.get().setEnabled(enabled);
    }

    /**
     * The lists, as JSON: [{id, title, url, builtin, enabled, urlRules, cssRules, skipped,
     * updatedMs, error, updating}].
     */
    static String listsJson() {
        return AdBlockBridgeJni.get().listsJson();
    }

    static void setListEnabled(String id, boolean enabled) {
        AdBlockBridgeJni.get().setListEnabled(id, enabled);
    }

    /** Adds the list at |url| (https): why not, empty when added. */
    static String addList(String url) {
        return AdBlockBridgeJni.get().addList(url);
    }

    static void removeList(String id) {
        AdBlockBridgeJni.get().removeList(id);
    }

    static void updateNow() {
        AdBlockBridgeJni.get().updateNow();
    }

    @NativeMethods
    interface Natives {
        long init(AdBlockBridge caller);

        void destroy(long nativeAdBlockBridge);

        boolean isEnabled();

        void setEnabled(boolean enabled);

        @JniType("std::string")
        String listsJson();

        void setListEnabled(@JniType("std::string") String id, boolean enabled);

        @JniType("std::string")
        String addList(@JniType("std::string") String url);

        void removeList(@JniType("std::string") String id);

        void updateNow();
    }
}
