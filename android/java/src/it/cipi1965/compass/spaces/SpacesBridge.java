// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.spaces;

import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.chrome.browser.profiles.Profile;

/**
 * Java side of the profile's spaces (//compass/browser/android: SpacesAndroid, the desktop's
 * model). Reads go through {@link #snapshot()}, JSON in the sidebar store's format; changes come
 * back as a callback.
 */
@JNINamespace("compass")
public class SpacesBridge {
    private long mNativeSpacesBridge;
    private final Runnable mOnChanged;

    public SpacesBridge(Profile profile, Runnable onChanged) {
        mOnChanged = onChanged;
        mNativeSpacesBridge = SpacesBridgeJni.get().init(this, profile);
    }

    public void destroy() {
        if (mNativeSpacesBridge == 0) return;
        SpacesBridgeJni.get().destroy(mNativeSpacesBridge);
        mNativeSpacesBridge = 0;
    }

    @CalledByNative
    private void onChanged() {
        mOnChanged.run();
    }

    /** The spaces, favorites and open tabs; see SpacesAndroid::Snapshot(). */
    public String snapshot() {
        return SpacesBridgeJni.get().snapshot(mNativeSpacesBridge);
    }

    public void setActiveSpace(String spaceId) {
        SpacesBridgeJni.get().setActiveSpace(mNativeSpacesBridge, spaceId);
    }

    /** Adds a space after the others, makes it active, and returns its ID. */
    public String addSpace(String name) {
        return SpacesBridgeJni.get().addSpace(mNativeSpacesBridge, name);
    }

    /** Edits a space: its name, its emoji (empty for none), its tint ("#rrggbb", empty: grey). */
    public void editSpace(String spaceId, String name, String emoji, String tint) {
        SpacesBridgeJni.get().editSpace(mNativeSpacesBridge, spaceId, name, emoji, tint);
    }

    /** Removes a space (not the last one); the space after it becomes active. */
    public void removeSpace(String spaceId) {
        SpacesBridgeJni.get().removeSpace(mNativeSpacesBridge, spaceId);
    }

    /** Pins a tab's page to the active space, or to the favorites. */
    public void pin(int tabId, boolean favorite) {
        SpacesBridgeJni.get().pin(mNativeSpacesBridge, tabId, favorite);
    }

    public void removeNode(String nodeId) {
        SpacesBridgeJni.get().removeNode(mNativeSpacesBridge, nodeId);
    }

    public void setCollapsed(String folderId, boolean collapsed) {
        SpacesBridgeJni.get().setCollapsed(mNativeSpacesBridge, folderId, collapsed);
    }

    /** The tab showing a node, or -1. */
    public int tabFor(String nodeId) {
        return SpacesBridgeJni.get().tabFor(mNativeSpacesBridge, nodeId);
    }

    public void tabOpened(int tabId, String url, String title, boolean restored) {
        SpacesBridgeJni.get().tabOpened(mNativeSpacesBridge, tabId, url, title, restored);
    }

    public void tabOpenedForNode(int tabId, String nodeId) {
        SpacesBridgeJni.get().tabOpenedForNode(mNativeSpacesBridge, tabId, nodeId);
    }

    public void tabUpdated(int tabId, String url, String title) {
        SpacesBridgeJni.get().tabUpdated(mNativeSpacesBridge, tabId, url, title);
    }

    public void tabSelected(int tabId) {
        SpacesBridgeJni.get().tabSelected(mNativeSpacesBridge, tabId);
    }

    public void tabClosed(int tabId) {
        SpacesBridgeJni.get().tabClosed(mNativeSpacesBridge, tabId);
    }

    @NativeMethods
    interface Natives {
        long init(SpacesBridge caller, @JniType("Profile*") Profile profile);

        void destroy(long nativeSpacesBridge);

        @JniType("std::string")
        String snapshot(long nativeSpacesBridge);

        void setActiveSpace(long nativeSpacesBridge, @JniType("std::string") String spaceId);

        @JniType("std::string")
        String addSpace(long nativeSpacesBridge, @JniType("std::u16string") String name);

        void editSpace(
                long nativeSpacesBridge,
                @JniType("std::string") String spaceId,
                @JniType("std::u16string") String name,
                @JniType("std::string") String emoji,
                @JniType("std::string") String tint);

        void removeSpace(long nativeSpacesBridge, @JniType("std::string") String spaceId);

        void pin(long nativeSpacesBridge, int tabId, boolean favorite);

        void removeNode(long nativeSpacesBridge, @JniType("std::string") String nodeId);

        void setCollapsed(
                long nativeSpacesBridge, @JniType("std::string") String folderId, boolean collapsed);

        int tabFor(long nativeSpacesBridge, @JniType("std::string") String nodeId);

        void tabOpened(
                long nativeSpacesBridge,
                int tabId,
                @JniType("std::string") String url,
                @JniType("std::u16string") String title,
                boolean restored);

        void tabOpenedForNode(
                long nativeSpacesBridge, int tabId, @JniType("std::string") String nodeId);

        void tabUpdated(
                long nativeSpacesBridge,
                int tabId,
                @JniType("std::string") String url,
                @JniType("std::u16string") String title);

        void tabSelected(long nativeSpacesBridge, int tabId);

        void tabClosed(long nativeSpacesBridge, int tabId);
    }
}
