// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.spaces;

import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.base.Callback;
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
    private final Callback<Integer> mOnTabArchived;

    /** A bridge that only reads (the start page): archived tabs are the window's to close. */
    public SpacesBridge(Profile profile, Runnable onChanged) {
        this(profile, onChanged, tabId -> {});
    }

    /**
     * @param onChanged runs when anything in the spaces changes.
     * @param onTabArchived gets each open tab whose Today entry the archiver archived; the tab
     *     should close (without undo).
     */
    public SpacesBridge(Profile profile, Runnable onChanged, Callback<Integer> onTabArchived) {
        mOnChanged = onChanged;
        mOnTabArchived = onTabArchived;
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

    @CalledByNative
    private void onTabArchived(int tabId) {
        mOnTabArchived.onResult(tabId);
    }

    /** The spaces, favorites and open tabs; see SpacesAndroid::Snapshot(). */
    public String snapshot() {
        return SpacesBridgeJni.get().snapshot(mNativeSpacesBridge);
    }

    /** The space this window shows (empty until the spaces are loaded). */
    public String shownSpace() {
        return SpacesBridgeJni.get().shownSpace(mNativeSpacesBridge);
    }

    /** Shows a space here without making it the one new windows start on. */
    public void showSpace(String spaceId) {
        SpacesBridgeJni.get().showSpace(mNativeSpacesBridge, spaceId);
    }

    /** Shows a space in this window; new windows start on it too. */
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

    /**
     * Moves an entry, with its tab, to a space's "pinned" tree or "today" (|spaceId| empty: this
     * window's space), or to the "favorites".
     */
    public void moveNode(String nodeId, String spaceId, String section) {
        SpacesBridgeJni.get().moveNode(mNativeSpacesBridge, nodeId, spaceId, section);
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

    /** A closed tab came back: its entry comes back out of the archive, or a new one is made. */
    public void tabClosureUndone(int tabId, String url, String title) {
        SpacesBridgeJni.get().tabClosureUndone(mNativeSpacesBridge, tabId, url, title);
    }

    /** Archives a Today entry that has no open tab (one with a tab is archived by closing it). */
    public void archiveNode(String nodeId) {
        SpacesBridgeJni.get().archiveNode(mNativeSpacesBridge, nodeId);
    }

    /**
     * Puts an archived entry back at the top of its space's Today, unloaded, and returns the node
     * to open (empty if none).
     */
    public String restoreArchived(String entryId) {
        return SpacesBridgeJni.get().restoreArchived(mNativeSpacesBridge, entryId);
    }

    public void removeArchived(String entryId) {
        SpacesBridgeJni.get().removeArchived(mNativeSpacesBridge, entryId);
    }

    /** Empties this window's space's archive. */
    public void clearArchive() {
        SpacesBridgeJni.get().clearArchive(mNativeSpacesBridge);
    }

    /** How long this window's space's Today entries stay unseen before the archive (0: never). */
    public void setArchiveAfter(int hours) {
        SpacesBridgeJni.get().setArchiveAfter(mNativeSpacesBridge, hours);
    }

    /** Makes a pinned tab's or favorite's current page its pinned URL. */
    public void useCurrentPageAsSavedUrl(String nodeId) {
        SpacesBridgeJni.get().useCurrentPageAsSavedUrl(mNativeSpacesBridge, nodeId);
    }

    /**
     * Returns a pinned tab's or favorite's pinned URL (empty if none); an entry with no tab goes
     * back to it here, an open tab is for the caller to load it in.
     */
    public String backToSavedUrl(String nodeId) {
        return SpacesBridgeJni.get().backToSavedUrl(mNativeSpacesBridge, nodeId);
    }

    @NativeMethods
    interface Natives {
        long init(SpacesBridge caller, @JniType("Profile*") Profile profile);

        void destroy(long nativeSpacesBridge);

        @JniType("std::string")
        String snapshot(long nativeSpacesBridge);

        @JniType("std::string")
        String shownSpace(long nativeSpacesBridge);

        void showSpace(long nativeSpacesBridge, @JniType("std::string") String spaceId);

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

        void moveNode(
                long nativeSpacesBridge,
                @JniType("std::string") String nodeId,
                @JniType("std::string") String spaceId,
                @JniType("std::string") String section);

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

        void tabClosureUndone(
                long nativeSpacesBridge,
                int tabId,
                @JniType("std::string") String url,
                @JniType("std::u16string") String title);

        void archiveNode(long nativeSpacesBridge, @JniType("std::string") String nodeId);

        @JniType("std::string")
        String restoreArchived(long nativeSpacesBridge, @JniType("std::string") String entryId);

        void removeArchived(long nativeSpacesBridge, @JniType("std::string") String entryId);

        void clearArchive(long nativeSpacesBridge);

        void setArchiveAfter(long nativeSpacesBridge, int hours);

        void useCurrentPageAsSavedUrl(
                long nativeSpacesBridge, @JniType("std::string") String nodeId);

        @JniType("std::string")
        String backToSavedUrl(long nativeSpacesBridge, @JniType("std::string") String nodeId);
    }
}
