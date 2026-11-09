// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.drawable.Icon;
import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.extras.CompassIntents;
import it.cipi1965.compass.spaces.SpacesBridge;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabCreationState;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.tab.TabSelectionType;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.tabmodel.TabModelObserver;
import org.chromium.chrome.browser.tabmodel.TabModelSelectorTabObserver;
import org.chromium.chrome.browser.tabmodel.TabModelUtils;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.ui.base.PageTransition;
import org.chromium.url.GURL;

import java.util.ArrayList;
import java.util.List;

/**
 * The profile's spaces in the shell (docs/design/android.md, C2 and C3): keeps the native model
 * (SpacesBridge) in step with Chrome's regular tabs, and runs the sidebar (SidebarUi.kt).
 * Incognito tabs aren't in spaces.
 */
public class Spaces implements SidebarUi.Listener {
    private final ChromeTabbedActivity mActivity;
    private final TabModel mTabs;
    private final SpacesBridge mBridge;
    private final SidebarState mState = new SidebarState();
    private final View mView;
    // The node a tab is being opened for; its didAddTab() maps it instead of adding to Today.
    private @Nullable String mOpeningNode;
    private @Nullable Runnable mOnChanged;
    // The spaces the launcher shortcuts were made for (id, emoji and name of each).
    private String mShortcutSpaces = "";

    public Spaces(ChromeTabbedActivity activity, ViewGroup parent) {
        mActivity = activity;
        mTabs = activity.getTabModelSelector().getModel(false);
        mBridge = new SpacesBridge(mTabs.getProfile(), this::onModelChanged);

        // Tabs restored before the shell came up.
        for (int i = 0; i < mTabs.getCount(); i++) {
            Tab tab = mTabs.getTabAt(i);
            mBridge.tabOpened(tab.getId(), tab.getUrl().getSpec(), tab.getTitle(), true);
        }
        mTabs.addObserver(
                new TabModelObserver() {
                    @Override
                    public void didAddTab(
                            Tab tab,
                            @TabLaunchType int type,
                            @TabCreationState int creationState,
                            boolean markedForSelection) {
                        if (mOpeningNode != null) {
                            mBridge.tabOpenedForNode(tab.getId(), mOpeningNode);
                            mOpeningNode = null;
                            return;
                        }
                        boolean restored =
                                type == TabLaunchType.FROM_RESTORE
                                        || creationState == TabCreationState.FROZEN_ON_RESTORE;
                        mBridge.tabOpened(
                                tab.getId(), tab.getUrl().getSpec(), tab.getTitle(), restored);
                    }

                    @Override
                    public void didSelectTab(Tab tab, @TabSelectionType int type, int lastId) {
                        mBridge.tabSelected(tab.getId());
                    }

                    @Override
                    public void didRemoveTabForClosure(Tab tab) {
                        mBridge.tabClosed(tab.getId());
                    }

                    @Override
                    public void tabRemoved(Tab tab) {
                        mBridge.tabClosed(tab.getId());
                    }
                });
        new TabModelSelectorTabObserver(activity.getTabModelSelector()) {
            @Override
            public void onUrlUpdated(Tab tab) {
                update(tab);
            }

            @Override
            public void onTitleUpdated(Tab tab) {
                update(tab);
            }

            private void update(Tab tab) {
                if (tab.isIncognito()) return;
                mBridge.tabUpdated(tab.getId(), tab.getUrl().getSpec(), tab.getTitle());
            }
        };

        mView = SidebarUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        onModelChanged();
    }

    /** Runs when the spaces change (the bar shows the active space's icon). */
    public void setOnChanged(Runnable onChanged) {
        mOnChanged = onChanged;
    }

    public SidebarState state() {
        return mState;
    }

    public void showSidebar() {
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
    }

    /** Pins the current tab to its space, or to the favorites. */
    public void pinCurrentTab(boolean favorite) {
        Tab tab = mActivity.getActivityTabProvider().get();
        if (tab != null && !tab.isIncognito()) mBridge.pin(tab.getId(), favorite);
    }

    /** Shows a space (a launcher shortcut's), with its sidebar. */
    public void openSpace(String spaceId) {
        mBridge.setActiveSpace(spaceId);
        showSidebar();
    }

    /** Opens |url| in a new tab and pins it to the active space (the share target). */
    public void openAndPin(String url) {
        GURL gurl = new GURL(url);
        if (!gurl.isValid()) return;
        Tab tab =
                mActivity
                        .getTabCreator(false)
                        .createNewTab(
                                new LoadUrlParams(gurl.getSpec(), PageTransition.LINK),
                                TabLaunchType.FROM_EXTERNAL_APP,
                                null);
        if (tab != null) mBridge.pin(tab.getId(), false);
    }

    private void onModelChanged() {
        mState.update(mBridge.snapshot(), currentTabId());
        updateShortcuts();
        if (mOnChanged != null) mOnChanged.run();
    }

    // A launcher shortcut per space (the first few), to open it.
    private void updateShortcuts() {
        StringBuilder key = new StringBuilder();
        for (SidebarState.Space space : mState.getSpaces()) {
            key.append(space.id).append(space.emoji).append(space.name).append('\n');
        }
        if (key.toString().equals(mShortcutSpaces)) return;
        mShortcutSpaces = key.toString();
        ShortcutManager manager = mActivity.getSystemService(ShortcutManager.class);
        if (manager == null) return;
        // Chrome's static shortcuts (new tab, new incognito tab) count too.
        int max = Math.max(0, manager.getMaxShortcutCountPerActivity() - 2);
        int icon =
                mActivity
                        .getResources()
                        .getIdentifier(
                                "compass_icon_monochrome", "drawable", mActivity.getPackageName());
        List<ShortcutInfo> shortcuts = new ArrayList<>();
        int rank = 0;
        for (SidebarState.Space space : mState.getSpaces()) {
            if (shortcuts.size() >= max) break;
            Intent intent = CompassIntents.create(mActivity, CompassIntents.ACTION_OPEN_SPACE);
            intent.putExtra(CompassIntents.EXTRA_SPACE_ID, space.id);
            String label = space.emoji.isEmpty() ? space.name : space.emoji + " " + space.name;
            shortcuts.add(
                    new ShortcutInfo.Builder(mActivity, "space-" + space.id)
                            .setShortLabel(label.isEmpty() ? "Space" : label)
                            .setIcon(Icon.createWithResource(mActivity, icon))
                            .setIntent(intent)
                            .setRank(rank++)
                            .build());
        }
        try {
            manager.setDynamicShortcuts(shortcuts);
        } catch (IllegalArgumentException | IllegalStateException e) {
            // Rate-limited or over the limit: the next change tries again.
            mShortcutSpaces = "";
        }
    }

    private int currentTabId() {
        Tab tab = mActivity.getActivityTabProvider().get();
        return tab != null ? tab.getId() : Tab.INVALID_TAB_ID;
    }

    // SidebarUi.Listener:

    @Override
    public void onNodeClicked(String nodeId, String url) {
        int tabId = mBridge.tabFor(nodeId);
        if (tabId != Tab.INVALID_TAB_ID && mTabs.getTabById(tabId) != null) {
            TabModelUtils.selectTabById(
                    mActivity.getTabModelSelector(), tabId, TabSelectionType.FROM_USER);
        } else {
            GURL gurl = new GURL(url);
            if (!gurl.isValid()) return;
            mOpeningNode = nodeId;
            mActivity
                    .getTabCreator(false)
                    .createNewTab(
                            new LoadUrlParams(gurl.getSpec(), PageTransition.AUTO_BOOKMARK),
                            TabLaunchType.FROM_CHROME_UI,
                            null);
            mOpeningNode = null;
        }
        onDismiss();
    }

    @Override
    public void onCloseNode(String nodeId) {
        int tabId = mBridge.tabFor(nodeId);
        Tab tab = tabId != Tab.INVALID_TAB_ID ? mTabs.getTabById(tabId) : null;
        if (tab != null) {
            // Closing the tab removes a Today entry; a pinned one stays, unloaded.
            mTabs.getTabRemover()
                    .closeTabs(
                            org.chromium.chrome.browser.tabmodel.TabClosureParams.closeTab(tab)
                                    .allowUndo(false)
                                    .build(),
                            /* allowDialog= */ false);
        } else {
            mBridge.removeNode(nodeId);
        }
    }

    @Override
    public void onUnpinNode(String nodeId) {
        mBridge.removeNode(nodeId);
    }

    @Override
    public void onToggleFolder(String folderId, boolean collapsed) {
        mBridge.setCollapsed(folderId, collapsed);
    }

    @Override
    public void onSpaceClicked(String spaceId) {
        mBridge.setActiveSpace(spaceId);
    }

    @Override
    public void onAddSpace(String name) {
        mBridge.addSpace(name);
    }

    @Override
    public void onNewTab() {
        onDismiss();
        mActivity.getTabCreator(false).launchNtp(TabLaunchType.FROM_CHROME_UI);
    }

    @Override
    public void onPinCurrentTab(boolean favorite) {
        pinCurrentTab(favorite);
    }

    @Override
    public void onDismiss() {
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
