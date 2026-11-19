// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.claude;

import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.content_public.browser.WebContents;

/**
 * Java side of the Claude chat (//compass/browser/android: ClaudeBridge, on the desktop's Claude
 * client): the conversation as JSON snapshots, questions and the key as calls.
 */
@JNINamespace("compass")
public class ClaudeBridge {
    /** The chat's window, for Claude's browser tools (ClaudeTabsAndroid::Window). */
    public interface Window {
        /** The space the window shows (its UUID), or "". */
        String shownSpace();

        /**
         * Shows the sidebar entry |nodeId| (a tab's UUID): its tab, or a new one for it; first
         * |spaceId|, its space ("" for a favorite), if the window shows another.
         */
        void showNode(String nodeId, String url, String spaceId);

        /** Opens |nodeId| in a background tab of the window. */
        void loadInBackground(String nodeId, String url);

        /** The window shows |spaceId|. */
        void switchSpace(String spaceId);
    }

    private long mNativeClaudeBridge;
    private final Runnable mOnChanged;
    private final Window mWindow;

    public ClaudeBridge(Profile profile, Window window, Runnable onChanged) {
        mOnChanged = onChanged;
        mWindow = window;
        mNativeClaudeBridge = ClaudeBridgeJni.get().init(this, profile);
    }

    public void destroy() {
        if (mNativeClaudeBridge == 0) return;
        ClaudeBridgeJni.get().destroy(mNativeClaudeBridge);
        mNativeClaudeBridge = 0;
    }

    @CalledByNative
    private void onChanged() {
        mOnChanged.run();
    }

    /**
     * The shared Downloads folder the Files app shows (Claude's files, docs/design/claude.md,
     * "Files"); Chromium's own downloads directory is the app's private one.
     */
    @CalledByNative
    private static @JniType("std::string") String publicDownloads() {
        return android.os.Environment.getExternalStoragePublicDirectory(
                        android.os.Environment.DIRECTORY_DOWNLOADS)
                .getAbsolutePath();
    }

    @CalledByNative
    private @JniType("std::string") String shownSpace() {
        return mWindow.shownSpace();
    }

    @CalledByNative
    private void showNode(
            @JniType("std::string") String nodeId,
            @JniType("std::string") String url,
            @JniType("std::string") String spaceId) {
        mWindow.showNode(nodeId, url, spaceId);
    }

    @CalledByNative
    private void loadInBackground(
            @JniType("std::string") String nodeId, @JniType("std::string") String url) {
        mWindow.loadInBackground(nodeId, url);
    }

    @CalledByNative
    private void switchSpace(@JniType("std::string") String spaceId) {
        mWindow.switchSpace(spaceId);
    }

    public String snapshot() {
        return ClaudeBridgeJni.get().snapshot(mNativeClaudeBridge);
    }

    public void setApiKey(String key) {
        ClaudeBridgeJni.get().setApiKey(mNativeClaudeBridge, key);
    }

    /** Asks |text| about the page |contents| shows. */
    public void send(String text, @Nullable WebContents contents) {
        ClaudeBridgeJni.get().send(mNativeClaudeBridge, text, contents);
    }

    /** "Let Claude use the whole browser": the tabs, not only the page. */
    public void setBrowserScope(boolean allowed) {
        ClaudeBridgeJni.get().setBrowserScope(mNativeClaudeBridge, allowed);
    }

    public void stop() {
        ClaudeBridgeJni.get().stop(mNativeClaudeBridge);
    }

    public void clear() {
        ClaudeBridgeJni.get().clear(mNativeClaudeBridge);
    }

    /** The model the next reply uses (empty: the default). */
    public void setModel(String model) {
        ClaudeBridgeJni.get().setModel(mNativeClaudeBridge, model);
    }

    /**
     * Claude in the sidebar: a feature's switch ("name_link_folders", "tidy_today",
     * "short_tab_titles", "command_bar_ask").
     */
    public void setFeature(String feature, boolean on) {
        ClaudeBridgeJni.get().setFeature(mNativeClaudeBridge, feature, on);
    }

    /** A feature's model. */
    public void setFeatureModel(String feature, String model) {
        ClaudeBridgeJni.get().setFeatureModel(mNativeClaudeBridge, feature, model);
    }

    /**
     * Continue in Chat: a new chat that starts with |question| (|sent|: as Claude got it) and
     * |answer|.
     */
    public void seed(String question, String sent, String answer) {
        ClaudeBridgeJni.get().seed(mNativeClaudeBridge, question, sent, answer);
    }

    /** "Use memory": what Claude remembers goes with each new chat. */
    public void setMemoryEnabled(boolean enabled) {
        ClaudeBridgeJni.get().setMemoryEnabled(mNativeClaudeBridge, enabled);
    }

    /** "Let Claude save memories": the memory tools go with each new chat. */
    public void setMemorySave(boolean enabled) {
        ClaudeBridgeJni.get().setMemorySave(mNativeClaudeBridge, enabled);
    }

    /** Takes back a change Claude made to its memory (a "memory" message's undo id). */
    public void undoMemory(int undoId) {
        ClaudeBridgeJni.get().undoMemory(mNativeClaudeBridge, undoId);
    }

    public void addMemory(String text) {
        ClaudeBridgeJni.get().addMemory(mNativeClaudeBridge, text);
    }

    /** The user's edit of a memory. */
    public void updateMemory(String id, String text) {
        ClaudeBridgeJni.get().updateMemory(mNativeClaudeBridge, id, text);
    }

    public void removeMemory(String id) {
        ClaudeBridgeJni.get().removeMemory(mNativeClaudeBridge, id);
    }

    public void removeAllMemories() {
        ClaudeBridgeJni.get().removeAllMemories(mNativeClaudeBridge);
    }

    /**
     * The endpoint: a base URL ("" for the Claude API) and how the key is sent (0 x-api-key, 1
     * Authorization: Bearer). False for a URL that isn't usable (the snapshot says why).
     */
    public boolean setEndpoint(String baseUrl, int authStyle) {
        return ClaudeBridgeJni.get().setEndpoint(mNativeClaudeBridge, baseUrl, authStyle);
    }

    /** Test Connection: lists the models the key can use. */
    public void testConnection() {
        ClaudeBridgeJni.get().testConnection(mNativeClaudeBridge);
    }

    /** Whether questions carry the page (each page once a chat). */
    public void setAttachPage(boolean attach) {
        ClaudeBridgeJni.get().setAttachPage(mNativeClaudeBridge, attach);
    }

    /** Asks the last question again after a failed reply. */
    public void retry() {
        ClaudeBridgeJni.get().retry(mNativeClaudeBridge);
    }

    /** A tool row's screenshot, base64 JPEG, or "". */
    public String toolImage(String toolUseId) {
        return ClaudeBridgeJni.get().toolImage(mNativeClaudeBridge, toolUseId);
    }

    /** "Accept the risks of subscription tokens": a `claude setup-token` token as the key. */
    public void setAcceptTokenRisks(boolean accepted) {
        ClaudeBridgeJni.get().setAcceptTokenRisks(mNativeClaudeBridge, accepted);
    }

    /** "Let Claude make Boosts". */
    public void setBoostsAllowed(boolean allowed) {
        ClaudeBridgeJni.get().setBoostsAllowed(mNativeClaudeBridge, allowed);
    }

    /** Takes back a change Claude made to a Boost (a "boost" message's undo id). */
    public void undoBoost(int undoId) {
        ClaudeBridgeJni.get().undoBoost(mNativeClaudeBridge, undoId);
    }

    public void setBoostEnabled(String id, boolean enabled) {
        ClaudeBridgeJni.get().setBoostEnabled(mNativeClaudeBridge, id, enabled);
    }

    public void removeBoost(String id) {
        ClaudeBridgeJni.get().removeBoost(mNativeClaudeBridge, id);
    }

    /** "Let Claude use developer tools". */
    public void setDevTools(boolean allowed) {
        ClaudeBridgeJni.get().setDevTools(mNativeClaudeBridge, allowed);
    }

    /** Files: 0 off, 1 save files (Downloads), 2 use |folder|, a folder of Downloads by name. */
    public void setFiles(int access, String folder) {
        ClaudeBridgeJni.get().setFiles(mNativeClaudeBridge, access, folder);
    }

    /** The user's answer to approval |id|: 0 deny, 1 allow, 2 allow the tool for this chat. */
    public void answer(int id, int decision) {
        ClaudeBridgeJni.get().answer(mNativeClaudeBridge, id, decision);
    }

    @NativeMethods
    interface Natives {
        long init(ClaudeBridge caller, @JniType("Profile*") Profile profile);

        void destroy(long nativeClaudeBridge);

        @JniType("std::string")
        String snapshot(long nativeClaudeBridge);

        void setApiKey(long nativeClaudeBridge, @JniType("std::string") String key);

        void send(
                long nativeClaudeBridge,
                @JniType("std::string") String text,
                @JniType("content::WebContents*") @Nullable WebContents contents);

        void setBrowserScope(long nativeClaudeBridge, boolean allowed);

        void stop(long nativeClaudeBridge);

        void clear(long nativeClaudeBridge);

        void setModel(long nativeClaudeBridge, @JniType("std::string") String model);

        void setFeature(
                long nativeClaudeBridge, @JniType("std::string") String feature, boolean on);

        void setFeatureModel(
                long nativeClaudeBridge,
                @JniType("std::string") String feature,
                @JniType("std::string") String model);

        void seed(
                long nativeClaudeBridge,
                @JniType("std::string") String question,
                @JniType("std::string") String sent,
                @JniType("std::string") String answer);

        void setMemoryEnabled(long nativeClaudeBridge, boolean enabled);

        void setMemorySave(long nativeClaudeBridge, boolean enabled);

        void undoMemory(long nativeClaudeBridge, int undoId);

        void addMemory(long nativeClaudeBridge, @JniType("std::string") String text);

        void updateMemory(
                long nativeClaudeBridge,
                @JniType("std::string") String id,
                @JniType("std::string") String text);

        void removeMemory(long nativeClaudeBridge, @JniType("std::string") String id);

        void removeAllMemories(long nativeClaudeBridge);

        void setAcceptTokenRisks(long nativeClaudeBridge, boolean accepted);

        boolean setEndpoint(
                long nativeClaudeBridge, @JniType("std::string") String baseUrl, int authStyle);

        void testConnection(long nativeClaudeBridge);

        void setAttachPage(long nativeClaudeBridge, boolean attach);

        void retry(long nativeClaudeBridge);

        @JniType("std::string")
        String toolImage(long nativeClaudeBridge, @JniType("std::string") String toolUseId);

        void setBoostsAllowed(long nativeClaudeBridge, boolean allowed);

        void undoBoost(long nativeClaudeBridge, int undoId);

        void setBoostEnabled(
                long nativeClaudeBridge, @JniType("std::string") String id, boolean enabled);

        void removeBoost(long nativeClaudeBridge, @JniType("std::string") String id);

        void setDevTools(long nativeClaudeBridge, boolean allowed);

        void setFiles(
                long nativeClaudeBridge, int access, @JniType("std::string") String folder);

        void answer(long nativeClaudeBridge, int id, int decision);
    }
}
