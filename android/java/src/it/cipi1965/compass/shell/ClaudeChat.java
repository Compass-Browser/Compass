// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.claude.ClaudeBridge;

import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.ui.base.PageTransition;
import org.chromium.url.GURL;

/** The Claude chat (ClaudeChatUi.kt) over ClaudeBridge, about the tab on screen. */
public class ClaudeChat implements ClaudeChatUi.Listener {
    private final ChromeTabbedActivity mActivity;
    private final ClaudeBridge mBridge;
    private final ClaudeChatState mState = new ClaudeChatState();
    private final View mView;
    private final View mPill;
    private final Spaces mSpaces;

    /**
     * |aboveBarPx|: how far over the bottom the "Claude is working…" pill floats (the bar's
     * height).
     */
    public ClaudeChat(ChromeTabbedActivity activity, ViewGroup parent, Spaces spaces, int aboveBarPx) {
        mActivity = activity;
        mSpaces = spaces;
        mBridge =
                new ClaudeBridge(
                        activity.getTabModelSelector().getModel(false).getProfile(),
                        new ClaudeBridge.Window() {
                            @Override
                            public String shownSpace() {
                                return spaces.shownSpaceId();
                            }

                            @Override
                            public void showNode(String nodeId, String url, String spaceId) {
                                spaces.showNode(nodeId, url, spaceId);
                            }

                            @Override
                            public void loadInBackground(String nodeId, String url) {
                                spaces.openInBackground(nodeId, url);
                            }

                            @Override
                            public void switchSpace(String spaceId) {
                                spaces.switchToSpace(spaceId);
                            }

                            @Override
                            public void openTab(String url) {
                                onOpenLink(url);
                            }
                        },
                        this::refresh);
        mView = ClaudeChatUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        // While Claude works with the sheet closed (iOS's pill).
        mPill = ClaudeChatUi.createWorkingPill(activity, activity, activity, this::show, this::onStop);
        mPill.setVisibility(View.GONE);
        CoordinatorLayout.LayoutParams pillParams =
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        pillParams.gravity = android.view.Gravity.BOTTOM;
        pillParams.bottomMargin = aboveBarPx;
        parent.addView(mPill, pillParams);
    }

    public void destroy() {
        mBridge.destroy();
        if (mPill.getParent() != null) ((ViewGroup) mPill.getParent()).removeView(mPill);
    }

    public void show() {
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
        refresh();
    }

    private void refresh() {
        boolean replyEnded = mState.update(mBridge.snapshot());
        // The tab Claude acts on gets an outline in the sidebar (iOS's), and the pill shows while
        // it works with the sheet closed.
        mSpaces.setClaudeTab(mState.getActingTab());
        boolean pill = mState.getStreaming() && !mState.getOpen();
        if (pill != (mPill.getVisibility() == View.VISIBLE)) {
            mPill.setVisibility(pill ? View.VISIBLE : View.GONE);
            if (pill) mPill.bringToFront();
        }
        // The queue goes on; after an error it waits for the user (Retry, or Send).
        if (replyEnded) sendQueued();
    }

    /**
     * While Claude replies, the text is queued; between replies it's sent, or (empty) the next
     * queued message is.
     */
    @Override
    public void onSend(String text) {
        if (mState.getStreaming()) {
            mState.queue(text);
        } else if (text.trim().isEmpty()) {
            sendQueued();
        } else {
            send(text);
        }
    }

    private void sendQueued() {
        if (mState.getStreaming()) return;
        String next = mState.takeQueued();
        if (next != null) send(next);
    }

    private void send(String text) {
        Tab tab = mActivity.getActivityTabProvider().get();
        // Incognito pages stay out of it.
        mBridge.send(text, tab != null && !tab.isIncognito() ? tab.getWebContents() : null);
    }

    @Override
    public void onEditQueued(int index) {
        mState.editQueued(index);
    }

    @Override
    public void onRemoveQueued(int index) {
        mState.removeQueued(index);
    }

    @Override
    public void onBrowserScope(boolean allowed) {
        mBridge.setBrowserScope(allowed);
    }

    @Override
    public void onStop() {
        mState.unqueueAll();
        mBridge.stop();
    }

    @Override
    public void onClear() {
        mState.clearQueue();
        mBridge.clear();
    }

    @Override
    public void onSetKey(String key) {
        mBridge.setApiKey(key);
    }

    @Override
    public void onSignInWithChatGpt() {
        mBridge.signInWithChatGpt();
    }

    @Override
    public void onSignOutOfChatGpt() {
        mBridge.signOutOfChatGpt();
    }

    @Override
    public void onForgetKey() {
        // An empty key removes the saved one (ClaudeCredentials).
        mBridge.setApiKey("");
    }

    @Override
    public void onSetModel(String model) {
        mBridge.setModel(model);
    }

    @Override
    public void onSetProvider(String provider) {
        mBridge.setProvider(provider);
    }

    @Override
    public void onUndoMemory(int undoId) {
        mBridge.undoMemory(undoId);
    }

    @Override
    public void onMemoryEnabled(boolean enabled) {
        mBridge.setMemoryEnabled(enabled);
    }

    @Override
    public void onMemorySave(boolean enabled) {
        mBridge.setMemorySave(enabled);
    }

    @Override
    public void onAddMemory(String text) {
        mBridge.addMemory(text);
    }

    @Override
    public void onUpdateMemory(String id, String text) {
        mBridge.updateMemory(id, text);
    }

    @Override
    public void onRemoveMemory(String id) {
        mBridge.removeMemory(id);
    }

    @Override
    public void onRemoveAllMemories() {
        mBridge.removeAllMemories();
    }

    @Override
    public void onSetFeature(String feature, boolean on) {
        mBridge.setFeature(feature, on);
    }

    @Override
    public void onSetFeatureModel(String feature, String model) {
        mBridge.setFeatureModel(feature, model);
    }

    @Override
    public void onAcceptTokenRisks(boolean accepted) {
        mBridge.setAcceptTokenRisks(accepted);
    }

    @Override
    public void onBoostsAllowed(boolean allowed) {
        mBridge.setBoostsAllowed(allowed);
    }

    @Override
    public void onUndoBoost(int undoId) {
        mBridge.undoBoost(undoId);
    }

    @Override
    public void onBoostEnabled(String id, boolean enabled) {
        mBridge.setBoostEnabled(id, enabled);
    }

    @Override
    public void onRemoveBoost(String id) {
        mBridge.removeBoost(id);
    }

    @Override
    public void onDevTools(boolean allowed) {
        mBridge.setDevTools(allowed);
    }

    @Override
    public void onFiles(int access, String folder) {
        mBridge.setFiles(access, folder);
    }

    @Override
    public void onAnswer(int id, int decision) {
        mBridge.answer(id, decision);
    }

    /**
     * Continue in Chat (Ask in the command bar): the sheet, on a new chat that starts with the
     * question and Claude's answer.
     */
    public void continueWith(String question, String sent, String answer) {
        mBridge.seed(question, sent, answer);
        show();
    }

    @Override
    public void onOpenLink(String url) {
        GURL gurl = new GURL(url);
        if (!gurl.isValid()) return;
        // A new tab in front, as the desktop's chat opens links; the sheet steps aside.
        mActivity
                .getTabCreator(false)
                .createNewTab(
                        new LoadUrlParams(gurl.getSpec(), PageTransition.LINK),
                        TabLaunchType.FROM_CHROME_UI,
                        null);
        onDismiss();
    }

    @Override
    public void onAttachPage(boolean attach) {
        mBridge.setAttachPage(attach);
    }

    @Override
    public void onRetry() {
        mBridge.retry();
    }

    @Override
    public void onSetEndpoint(String baseUrl, int authStyle) {
        mBridge.setEndpoint(baseUrl, authStyle);
    }

    @Override
    public void onTestConnection() {
        mBridge.testConnection();
    }

    @Override
    public String toolImage(String toolId) {
        return mBridge.toolImage(toolId);
    }

    @Override
    public void onDismiss() {
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
        refresh();
    }
}
