// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.claude;

import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.chrome.browser.profiles.Profile;

/**
 * Java side of Claude in the sidebar (//compass/browser/android: ClaudeSidebarBridge, on the
 * desktop's LinkFolderNamer, TodayTidier, ShortTitler and ClaudeQuick; docs/design/ai-sidebar.md):
 * Suggest a Name, Tidy Today with Undo, and Ask in the command bar. One per window, on its regular
 * profile.
 */
@JNINamespace("compass")
public class ClaudeSidebarBridge {
    /** What happens, as it happens. */
    public interface Listener {
        /** What's offered changed: snapshot() again. */
        void onChanged();

        /** Claude just named a folder: its row sparkles. */
        void onFolderNamed(String folderId);

        /** A space was tidied into |folders| folders; undoTidy(|undoId|) takes it back. */
        void onTidied(String spaceId, int undoId, int folders);

        /** Tidying a space didn't work, or found nothing to group. */
        void onTidyFailed(String spaceId, String message);

        /** Ask's answer so far. */
        void onAnswer(String text);

        /** Ask's answer is done: all of it, or the error (empty: none). */
        void onAnswerDone(String text, String error);
    }

    private long mNativeClaudeSidebarBridge;
    private final Listener mListener;

    public ClaudeSidebarBridge(Profile profile, Listener listener) {
        mListener = listener;
        mNativeClaudeSidebarBridge = ClaudeSidebarBridgeJni.get().init(this, profile);
    }

    public void destroy() {
        if (mNativeClaudeSidebarBridge == 0) return;
        ClaudeSidebarBridgeJni.get().destroy(mNativeClaudeSidebarBridge);
        mNativeClaudeSidebarBridge = 0;
    }

    /** {"suggest_names", "tidy", "busy": [space ids], "ask"} (claude_sidebar_bridge.h). */
    public String snapshot() {
        if (mNativeClaudeSidebarBridge == 0) return "{}";
        return ClaudeSidebarBridgeJni.get().snapshot(mNativeClaudeSidebarBridge);
    }

    public void suggestName(String folderId) {
        ClaudeSidebarBridgeJni.get().suggestName(mNativeClaudeSidebarBridge, folderId);
    }

    public void tidy(String spaceId) {
        ClaudeSidebarBridgeJni.get().tidy(mNativeClaudeSidebarBridge, spaceId);
    }

    public boolean undoTidy(int undoId) {
        return ClaudeSidebarBridgeJni.get().undoTidy(mNativeClaudeSidebarBridge, undoId);
    }

    public boolean looksLikeQuestion(String text) {
        if (mNativeClaudeSidebarBridge == 0) return false;
        return ClaudeSidebarBridgeJni.get().looksLikeQuestion(mNativeClaudeSidebarBridge, text);
    }

    /**
     * Asks |question|, with the page's title and host for "About this page" (empty: none). Returns
     * the message Claude gets, for Continue in Chat; empty if it couldn't ask.
     */
    public String ask(String question, String pageTitle, String pageHost) {
        return ClaudeSidebarBridgeJni.get()
                .ask(mNativeClaudeSidebarBridge, question, pageTitle, pageHost);
    }

    public void cancelAsk() {
        if (mNativeClaudeSidebarBridge == 0) return;
        ClaudeSidebarBridgeJni.get().cancelAsk(mNativeClaudeSidebarBridge);
    }

    @CalledByNative
    private void onChanged() {
        mListener.onChanged();
    }

    @CalledByNative
    private void onFolderNamed(@JniType("std::string") String folderId) {
        mListener.onFolderNamed(folderId);
    }

    @CalledByNative
    private void onTidied(@JniType("std::string") String spaceId, int undoId, int folders) {
        mListener.onTidied(spaceId, undoId, folders);
    }

    @CalledByNative
    private void onTidyFailed(
            @JniType("std::string") String spaceId, @JniType("std::string") String message) {
        mListener.onTidyFailed(spaceId, message);
    }

    @CalledByNative
    private void onAnswer(@JniType("std::string") String text) {
        mListener.onAnswer(text);
    }

    @CalledByNative
    private void onAnswerDone(
            @JniType("std::string") String text, @JniType("std::string") String error) {
        mListener.onAnswerDone(text, error);
    }

    @NativeMethods
    interface Natives {
        long init(ClaudeSidebarBridge caller, @JniType("Profile*") Profile profile);

        void destroy(long nativeClaudeSidebarBridge);

        @JniType("std::string")
        String snapshot(long nativeClaudeSidebarBridge);

        void suggestName(long nativeClaudeSidebarBridge, @JniType("std::string") String folderId);

        void tidy(long nativeClaudeSidebarBridge, @JniType("std::string") String spaceId);

        boolean undoTidy(long nativeClaudeSidebarBridge, int undoId);

        boolean looksLikeQuestion(
                long nativeClaudeSidebarBridge, @JniType("std::u16string") String text);

        @JniType("std::string")
        String ask(
                long nativeClaudeSidebarBridge,
                @JniType("std::string") String question,
                @JniType("std::string") String pageTitle,
                @JniType("std::string") String pageHost);

        void cancelAsk(long nativeClaudeSidebarBridge);
    }
}
