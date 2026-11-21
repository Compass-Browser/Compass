// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.sync.RoomsBridge;

import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * The share sheet of a space, pinned folder or easel, and the join screen of an invite link
 * (ShareSheetUi.kt), over the profile's rooms (RoomsBridge; docs/design/sync.md, "Rooms:
 * clients").
 */
public class ShareSheet implements ShareSheetUi.Listener {
    private final ChromeTabbedActivity mActivity;
    private final RoomsBridge mBridge;
    private final ShareSheetState mState = new ShareSheetState();
    private final View mView;
    private final Runnable mOnRoomsChanged;

    public ShareSheet(ChromeTabbedActivity activity, ViewGroup parent, Runnable onRoomsChanged) {
        mActivity = activity;
        mOnRoomsChanged = onRoomsChanged;
        mBridge =
                new RoomsBridge(
                        activity.getTabModelSelector().getModel(false).getProfile(),
                        this::onRoomsChanged);
        mView = ShareSheetUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    public void destroy() {
        mBridge.destroy();
    }

    public RoomsBridge bridge() {
        return mBridge;
    }

    /** The share sheet of |id|: kind "space", "folder" or "easel". */
    public void show(String kind, String id) {
        mState.setError("");
        mState.setLink("");
        mState.setKind(kind);
        mState.setItemId(id);
        mState.setItem(mBridge.item(kind, id));
        mState.setDisplayName(mBridge.getDisplayName());
        mState.setMode("sheet");
        bringUp();
        loadRoom();
    }

    /** The join screen of an invite link (https://compassbrowser.org/join#… or compass://join#…). */
    public void join(String link) {
        mState.setError("");
        mState.setPreview(link, mBridge.previewJoin(link), mBridge.spaces());
        mState.setDisplayName(mBridge.getDisplayName());
        mState.setMode("join");
        bringUp();
    }

    private void bringUp() {
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
    }

    private void onRoomsChanged() {
        if ("sheet".equals(mState.getMode()) && !mState.getRoomId().isEmpty()) loadRoom();
        if (mOnRoomsChanged != null) mOnRoomsChanged.run();
    }

    private void loadRoom() {
        if (mState.getRoomId().isEmpty()) return;
        mBridge.getRoom(mState.getRoomId(), json -> mState.setRoom(json));
    }

    private static String errorOf(String json) {
        try {
            return new JSONObject(json).optString("error");
        } catch (JSONException e) {
            return "";
        }
    }

    /** Shows the answer's error, or reads the room again. */
    private void afterChange(String json) {
        mState.setBusy(false);
        String error = errorOf(json);
        mState.setError(error);
        if (error.isEmpty()) loadRoom();
    }

    @Override
    public void onShare() {
        mState.setBusy(true);
        mBridge.share(
                mState.getKind(),
                mState.getItemId(),
                json -> {
                    mState.setBusy(false);
                    String error = errorOf(json);
                    mState.setError(error);
                    if (!error.isEmpty()) return;
                    try {
                        mState.setRoomId(new JSONObject(json).optString("room_id"));
                    } catch (JSONException e) {
                        return;
                    }
                    loadRoom();
                });
    }

    @Override
    public void onCopyLink(String role, int expiresDays, int uses) {
        mState.setBusy(true);
        mBridge.createInviteLink(
                mState.getRoomId(),
                role,
                expiresDays,
                uses,
                json -> {
                    mState.setBusy(false);
                    String error = errorOf(json);
                    mState.setError(error);
                    if (!error.isEmpty()) return;
                    String link;
                    try {
                        link = new JSONObject(json).optString("link");
                    } catch (JSONException e) {
                        return;
                    }
                    ClipboardManager clipboard =
                            (ClipboardManager) mActivity.getSystemService(Context.CLIPBOARD_SERVICE);
                    clipboard.setPrimaryClip(ClipData.newPlainText("Compass invite link", link));
                    mState.setLink(link);
                    loadRoom();
                });
    }

    @Override
    public void onInviteByEmail(String email, String role) {
        mState.setBusy(true);
        mBridge.inviteByEmail(mState.getRoomId(), email, role, this::afterChange);
    }

    @Override
    public void onChangeRole(String clientId, String role) {
        mBridge.changeRole(mState.getRoomId(), clientId, role, this::afterChange);
    }

    @Override
    public void onRemoveMember(String clientId) {
        mState.setBusy(true);
        mBridge.removeMember(mState.getRoomId(), clientId, this::afterChange);
    }

    @Override
    public void onRevokeInvite(String inviteId) {
        mBridge.revokeInvite(mState.getRoomId(), inviteId, this::afterChange);
    }

    @Override
    public void onLeaveOrDelete() {
        mState.setBusy(true);
        String roomId = mState.getRoomId();
        org.chromium.base.Callback<String> done =
                json -> {
                    mState.setBusy(false);
                    String error = errorOf(json);
                    mState.setError(error);
                    if (error.isEmpty()) onDismiss();
                };
        if ("owner".equals(mState.getRole())) {
            mBridge.deleteRoom(roomId, done);
        } else {
            mBridge.leave(roomId, done);
        }
    }

    @Override
    public void onSetDisplayName(String name) {
        mBridge.setDisplayName(name);
        mState.setDisplayName(mBridge.getDisplayName());
    }

    @Override
    public void onJoin() {
        mState.setBusy(true);
        mState.setError("");
        mBridge.join(
                mState.getJoinLink(),
                mState.getJoinSpace(),
                json -> {
                    mState.setBusy(false);
                    String error = errorOf(json);
                    mState.setError(error);
                    if (error.isEmpty()) mState.setJoined(true);
                });
    }

    @Override
    public void onDismiss() {
        mState.setMode("closed");
        mView.setVisibility(View.GONE);
    }

    /** Whether the sheet or join screen is up (the back key closes it). */
    public boolean isShowing() {
        return !"closed".equals(mState.getMode());
    }
}
