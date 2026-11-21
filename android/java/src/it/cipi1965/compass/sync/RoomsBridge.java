// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.sync;

import android.util.SparseArray;

import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.base.Callback;
import org.chromium.chrome.browser.profiles.Profile;

/**
 * Java side of sharing with other people (//compass/browser/android: RoomsBridge, over the
 * desktop's RoomService; docs/design/sync.md, "Rooms: clients"). Answers are JSON; slow ones come
 * back through a callback.
 */
@JNINamespace("compass")
public class RoomsBridge {
    private long mNativeRoomsBridge;
    private final Runnable mOnChanged;
    private final SparseArray<Callback<String>> mPending = new SparseArray<>();
    private int mNextRequest = 1;

    public RoomsBridge(Profile profile, Runnable onChanged) {
        mOnChanged = onChanged;
        mNativeRoomsBridge = RoomsBridgeJni.get().init(this, profile);
    }

    public void destroy() {
        if (mNativeRoomsBridge == 0) return;
        RoomsBridgeJni.get().destroy(mNativeRoomsBridge);
        mNativeRoomsBridge = 0;
        mPending.clear();
    }

    @CalledByNative
    private void onChanged() {
        mOnChanged.run();
    }

    @CalledByNative
    private void onResult(int request, @JniType("std::string") String json) {
        Callback<String> callback = mPending.get(request);
        mPending.remove(request);
        if (callback != null) callback.onResult(json);
    }

    private int track(Callback<String> callback) {
        int request = mNextRequest++;
        mPending.put(request, callback);
        return request;
    }

    /** {"available", "notices": [{"room_id", "text"}]}. */
    public String snapshot() {
        return mNativeRoomsBridge == 0 ? "{}" : RoomsBridgeJni.get().snapshot(mNativeRoomsBridge);
    }

    /** {"kind", "id", "name", "room_id", "can_share"}, or "" if it isn't here. */
    public String item(String kind, String id) {
        return RoomsBridgeJni.get().item(mNativeRoomsBridge, kind, id);
    }

    /** {"shared", "view_only", "tooltip"}. */
    public String nodeState(String id) {
        return RoomsBridgeJni.get().nodeState(mNativeRoomsBridge, id);
    }

    public String spaceState(String id) {
        return RoomsBridgeJni.get().spaceState(mNativeRoomsBridge, id);
    }

    public void share(String kind, String id, Callback<String> callback) {
        RoomsBridgeJni.get().share(mNativeRoomsBridge, track(callback), kind, id);
    }

    public void getRoom(String roomId, Callback<String> callback) {
        RoomsBridgeJni.get().getRoom(mNativeRoomsBridge, track(callback), roomId);
    }

    public void createInviteLink(
            String roomId, String role, int expiresDays, int uses, Callback<String> callback) {
        RoomsBridgeJni.get()
                .createInviteLink(
                        mNativeRoomsBridge, track(callback), roomId, role, expiresDays, uses);
    }

    public void revokeInvite(String roomId, String inviteId, Callback<String> callback) {
        RoomsBridgeJni.get().revokeInvite(mNativeRoomsBridge, track(callback), roomId, inviteId);
    }

    public void inviteByEmail(String roomId, String email, String role, Callback<String> callback) {
        RoomsBridgeJni.get()
                .inviteByEmail(mNativeRoomsBridge, track(callback), roomId, email, role);
    }

    public void changeRole(String roomId, String clientId, String role, Callback<String> callback) {
        RoomsBridgeJni.get()
                .changeRole(mNativeRoomsBridge, track(callback), roomId, clientId, role);
    }

    public void removeMember(String roomId, String clientId, Callback<String> callback) {
        RoomsBridgeJni.get().removeMember(mNativeRoomsBridge, track(callback), roomId, clientId);
    }

    public void leave(String roomId, Callback<String> callback) {
        RoomsBridgeJni.get().leave(mNativeRoomsBridge, track(callback), roomId);
    }

    public void deleteRoom(String roomId, Callback<String> callback) {
        RoomsBridgeJni.get().deleteRoom(mNativeRoomsBridge, track(callback), roomId);
    }

    /** {"name", "kind", "server", "already_joined"}, or "" for a link that doesn't work. */
    public String previewJoin(String link) {
        return RoomsBridgeJni.get().previewJoin(mNativeRoomsBridge, link);
    }

    /** [{"id", "name", "active"}]. */
    public String spaces() {
        return RoomsBridgeJni.get().spaces(mNativeRoomsBridge);
    }

    public void join(String link, String spaceId, Callback<String> callback) {
        RoomsBridgeJni.get().join(mNativeRoomsBridge, track(callback), link, spaceId);
    }

    public String getDisplayName() {
        return RoomsBridgeJni.get().getDisplayName(mNativeRoomsBridge);
    }

    public void setDisplayName(String name) {
        RoomsBridgeJni.get().setDisplayName(mNativeRoomsBridge, name);
    }

    public void dismissNotice(String roomId) {
        RoomsBridgeJni.get().dismissNotice(mNativeRoomsBridge, roomId);
    }

    @NativeMethods
    interface Natives {
        long init(RoomsBridge caller, @JniType("Profile*") Profile profile);

        void destroy(long nativeRoomsBridge);

        @JniType("std::string")
        String snapshot(long nativeRoomsBridge);

        @JniType("std::string")
        String item(
                long nativeRoomsBridge,
                @JniType("std::string") String kind,
                @JniType("std::string") String id);

        @JniType("std::string")
        String nodeState(long nativeRoomsBridge, @JniType("std::string") String id);

        @JniType("std::string")
        String spaceState(long nativeRoomsBridge, @JniType("std::string") String id);

        void share(
                long nativeRoomsBridge,
                int request,
                @JniType("std::string") String kind,
                @JniType("std::string") String id);

        void getRoom(long nativeRoomsBridge, int request, @JniType("std::string") String roomId);

        void createInviteLink(
                long nativeRoomsBridge,
                int request,
                @JniType("std::string") String roomId,
                @JniType("std::string") String role,
                int expiresDays,
                int uses);

        void revokeInvite(
                long nativeRoomsBridge,
                int request,
                @JniType("std::string") String roomId,
                @JniType("std::string") String inviteId);

        void inviteByEmail(
                long nativeRoomsBridge,
                int request,
                @JniType("std::string") String roomId,
                @JniType("std::string") String email,
                @JniType("std::string") String role);

        void changeRole(
                long nativeRoomsBridge,
                int request,
                @JniType("std::string") String roomId,
                @JniType("std::string") String clientId,
                @JniType("std::string") String role);

        void removeMember(
                long nativeRoomsBridge,
                int request,
                @JniType("std::string") String roomId,
                @JniType("std::string") String clientId);

        void leave(long nativeRoomsBridge, int request, @JniType("std::string") String roomId);

        void deleteRoom(
                long nativeRoomsBridge, int request, @JniType("std::string") String roomId);

        @JniType("std::string")
        String previewJoin(long nativeRoomsBridge, @JniType("std::string") String link);

        @JniType("std::string")
        String spaces(long nativeRoomsBridge);

        void join(
                long nativeRoomsBridge,
                int request,
                @JniType("std::string") String link,
                @JniType("std::string") String spaceId);

        @JniType("std::string")
        String getDisplayName(long nativeRoomsBridge);

        void setDisplayName(long nativeRoomsBridge, @JniType("std::string") String name);

        void dismissNotice(long nativeRoomsBridge, @JniType("std::string") String roomId);
    }
}
