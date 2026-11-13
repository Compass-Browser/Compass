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
    private long mNativeClaudeBridge;
    private final Runnable mOnChanged;

    public ClaudeBridge(Profile profile, Runnable onChanged) {
        mOnChanged = onChanged;
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

    public void removeMemory(String id) {
        ClaudeBridgeJni.get().removeMemory(mNativeClaudeBridge, id);
    }

    public void removeAllMemories() {
        ClaudeBridgeJni.get().removeAllMemories(mNativeClaudeBridge);
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

        void setMemoryEnabled(long nativeClaudeBridge, boolean enabled);

        void setMemorySave(long nativeClaudeBridge, boolean enabled);

        void undoMemory(long nativeClaudeBridge, int undoId);

        void addMemory(long nativeClaudeBridge, @JniType("std::string") String text);

        void removeMemory(long nativeClaudeBridge, @JniType("std::string") String id);

        void removeAllMemories(long nativeClaudeBridge);
    }
}
