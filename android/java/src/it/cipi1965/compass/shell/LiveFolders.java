// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.spaces.SpacesBridge;

import org.chromium.chrome.browser.ChromeTabbedActivity;

/**
 * Live folders in the shell (docs/design/live-folders.md): the sheet for New Live Folder, Edit
 * Source and installing plugins (LiveFoldersUi.kt), on the shared C++ LiveFolderService that
 * SpacesAndroid runs (through SpacesBridge). The sidebar's own live folder actions (refresh, mark
 * all seen, convert) go straight to the bridge (Spaces.java).
 */
public class LiveFolders implements LiveFoldersUi.Listener {
    private final SpacesBridge mBridge;
    private final LiveFoldersState mState = new LiveFoldersState();
    private final View mView;
    // Answers for an earlier opening of the sheet are dropped.
    private int mSession;

    public LiveFolders(ChromeTabbedActivity activity, ViewGroup parent, SpacesBridge bridge) {
        mBridge = bridge;
        mView = LiveFoldersUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    /** New Live Folder: it goes at the end of this window's space's Pinned. */
    public void newFolder() {
        open("");
    }

    /** Edit Source of a live folder. */
    public void editSource(String folderId) {
        open(folderId);
    }

    private void open(String folderId) {
        mState.start(folderId);
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
        loadPlugins();
    }

    private void loadPlugins() {
        int session = ++mSession;
        mBridge.livePlugins(
                json -> {
                    if (session != mSession) return;
                    mState.setPluginsJson(json);
                    if (mState.getEditing()) {
                        mState.setSourceJson(mBridge.liveFolderSource(mState.getFolderId()));
                    }
                });
    }

    // LiveFoldersUi.Listener:

    @Override
    public void onSave() {
        LiveFoldersState.Plugin plugin = mState.getPlugin();
        if (plugin == null || mState.getSaving()) return;
        String error =
                mBridge.saveLiveFolder(
                        mState.getFolderId(),
                        plugin.id,
                        mState.getTitle().trim(),
                        mState.configJson(),
                        mState.secretsJson(),
                        mState.getGrantCookies());
        if (error.isEmpty()) {
            onDismiss();
        } else {
            mState.setError(error);
        }
    }

    @Override
    public void onReview(String url) {
        int session = mSession;
        mState.setError("");
        mState.setSaving(true);
        mBridge.previewLivePlugin(
                url,
                json -> {
                    if (session != mSession) return;
                    mState.setSaving(false);
                    mState.setPreviewJson(json);
                });
    }

    @Override
    public void onInstall() {
        LiveFoldersState.Preview preview = mState.getPreview();
        if (preview == null || mState.getSaving()) return;
        int session = mSession;
        mState.setSaving(true);
        mBridge.installLivePlugin(
                preview.source,
                preview.sha256,
                error -> {
                    if (session != mSession) return;
                    mState.setSaving(false);
                    if (!error.isEmpty()) {
                        mState.setError(error);
                        return;
                    }
                    // Back to the folder being made or edited, with the new plugin.
                    String id = preview.id;
                    int next = ++mSession;
                    mState.setInstalling(false);
                    mState.setPreview(null);
                    mState.setError("");
                    mState.setMissingPluginUrl("");
                    mState.setMissingPluginSha256("");
                    mState.setLoading(true);
                    mBridge.livePlugins(
                            json -> {
                                if (next != mSession) return;
                                mState.setPluginsJson(json);
                                if (mState.getEditing()) {
                                    mState.setSourceJson(
                                            mBridge.liveFolderSource(mState.getFolderId()));
                                    return;
                                }
                                for (LiveFoldersState.Plugin p : mState.getPlugins()) {
                                    if (p.id.equals(id)) mState.pick(p);
                                }
                            });
                });
    }

    @Override
    public void onRemovePlugin(String pluginId) {
        int session = mSession;
        mBridge.removeLivePlugin(
                pluginId,
                ignored -> {
                    if (session != mSession) return;
                    loadPlugins();
                });
    }

    @Override
    public void onDismiss() {
        ++mSession;
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
