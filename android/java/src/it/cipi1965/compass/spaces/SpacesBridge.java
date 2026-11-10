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

    // Live folders (docs/design/live-folders.md). Errors are messages for the user; empty: none.

    /**
     * The plugins a live folder can use, as a JSON array: {id, name, description, builtin, local,
     * config: [{key, type ("string", "url", "number", "bool", "select"), label, placeholder,
     * optional, default?, min?, max?, options: [{value, label}]}], secrets: [{key, label, help,
     * optional, set}], hosts, cookieHosts}.
     */
    public void livePlugins(Callback<String> callback) {
        if (mNativeSpacesBridge == 0) return;
        SpacesBridgeJni.get().livePlugins(mNativeSpacesBridge, callback);
    }

    /**
     * A live folder's source: {pluginId, title, config, cookiesGranted, pluginUrl, pluginSha256};
     * empty if it isn't one.
     */
    public String liveFolderSource(String folderId) {
        return SpacesBridgeJni.get().liveFolderSource(mNativeSpacesBridge, folderId);
    }

    /**
     * Makes a live folder at the end of this window's space's Pinned (|folderId| empty), or changes
     * a live folder's source. |configJson|: the config object; |secretsJson|: {key: value} of
     * secrets to store (empty values keep stored ones).
     */
    public String saveLiveFolder(
            String folderId,
            String pluginId,
            String title,
            String configJson,
            String secretsJson,
            boolean grantCookies) {
        return SpacesBridgeJni.get()
                .saveLiveFolder(
                        mNativeSpacesBridge,
                        folderId,
                        pluginId,
                        title,
                        configJson,
                        secretsJson,
                        grantCookies);
    }

    public void refreshLiveFolder(String folderId) {
        SpacesBridgeJni.get().refreshLiveFolder(mNativeSpacesBridge, folderId);
    }

    public void markAllSeen(String folderId) {
        SpacesBridgeJni.get().markAllSeen(mNativeSpacesBridge, folderId);
    }

    /** Makes a live folder a normal one: its entries become pinned tabs. */
    public void convertLiveFolder(String folderId) {
        SpacesBridgeJni.get().convertLiveFolder(mNativeSpacesBridge, folderId);
    }

    /**
     * The plugin whose manifest is at |url|, for review: {"preview": {id, name, description,
     * version, hosts, cookieHosts, secrets (labels), source, sha256}} or {"error": message}.
     */
    public void previewLivePlugin(String url, Callback<String> callback) {
        if (mNativeSpacesBridge == 0) return;
        SpacesBridgeJni.get().previewLivePlugin(mNativeSpacesBridge, url, callback);
    }

    /** Installs the reviewed plugin, if it's still what was reviewed; the callback gets an error. */
    public void installLivePlugin(String url, String sha256, Callback<String> callback) {
        if (mNativeSpacesBridge == 0) return;
        SpacesBridgeJni.get().installLivePlugin(mNativeSpacesBridge, url, sha256, callback);
    }

    /** Removes an installed plugin and its secrets; its folders stay. */
    public void removeLivePlugin(String pluginId, Callback<String> callback) {
        if (mNativeSpacesBridge == 0) return;
        SpacesBridgeJni.get().removeLivePlugin(mNativeSpacesBridge, pluginId, callback);
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

        void livePlugins(long nativeSpacesBridge, Callback<String> callback);

        @JniType("std::string")
        String liveFolderSource(long nativeSpacesBridge, @JniType("std::string") String folderId);

        @JniType("std::string")
        String saveLiveFolder(
                long nativeSpacesBridge,
                @JniType("std::string") String folderId,
                @JniType("std::string") String pluginId,
                @JniType("std::u16string") String title,
                @JniType("std::string") String configJson,
                @JniType("std::string") String secretsJson,
                boolean grantCookies);

        void refreshLiveFolder(long nativeSpacesBridge, @JniType("std::string") String folderId);

        void markAllSeen(long nativeSpacesBridge, @JniType("std::string") String folderId);

        void convertLiveFolder(long nativeSpacesBridge, @JniType("std::string") String folderId);

        void previewLivePlugin(
                long nativeSpacesBridge,
                @JniType("std::string") String url,
                Callback<String> callback);

        void installLivePlugin(
                long nativeSpacesBridge,
                @JniType("std::string") String url,
                @JniType("std::string") String sha256,
                Callback<String> callback);

        void removeLivePlugin(
                long nativeSpacesBridge,
                @JniType("std::string") String pluginId,
                Callback<String> callback);
    }
}
