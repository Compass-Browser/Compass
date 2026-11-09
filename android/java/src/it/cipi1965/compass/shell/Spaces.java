// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.drawable.Icon;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.extras.CompassIntents;
import it.cipi1965.compass.spaces.SpacesBridge;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.app.tabwindow.TabWindowManagerSingleton;
import org.chromium.chrome.browser.multiwindow.MultiInstanceOrchestratorFactory;
import org.chromium.chrome.browser.multiwindow.MultiWindowUtils;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabCreationState;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.tab.TabSelectionType;
import org.chromium.chrome.browser.tabmodel.TabList;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.tabmodel.TabModelObserver;
import org.chromium.chrome.browser.tabmodel.TabModelSelectorTabObserver;
import org.chromium.chrome.browser.tabmodel.TabModelUtils;
import org.chromium.chrome.browser.tabwindow.TabWindowInfo;
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
    // Windows at least this wide (Material's "expanded") show the sidebar as a pane.
    private static final int PANE_MIN_WINDOW_DP = 840;
    private static final int PANE_WIDTH_DP = 300;

    private final ChromeTabbedActivity mActivity;
    private final TabModel mTabs;
    private final SpacesBridge mBridge;
    private final SidebarState mState = new SidebarState();
    private final View mView;
    // The node a tab is being opened for; its didAddTab() maps it instead of adding to Today.
    private @Nullable String mOpeningNode;
    private @Nullable Runnable mOnChanged;
    private @Nullable Runnable mOnOpenSync;
    private @Nullable View mPane;
    private boolean mDocked;
    // Ctrl+S put the pane away (docked windows only).
    private boolean mPaneHidden;
    private @Nullable ViewGroup mCoordinator;
    private @Nullable FrameLayout mRoot;
    // The spaces the launcher shortcuts were made for (id, emoji and name of each).
    private String mShortcutSpaces = "";
    private final TabModelObserver mTabModelObserver;
    // A tab moving here from another window, selected when it arrives.
    private int mMovingTab = Tab.INVALID_TAB_ID;
    private final TabModelSelectorTabObserver mTabObserver;

    public Spaces(ChromeTabbedActivity activity, ViewGroup parent) {
        mActivity = activity;
        mTabs = activity.getTabModelSelector().getModel(false);
        mBridge = new SpacesBridge(mTabs.getProfile(), this::onModelChanged);

        // Tabs restored before the shell came up.
        for (int i = 0; i < mTabs.getCount(); i++) {
            Tab tab = mTabs.getTabAt(i);
            mBridge.tabOpened(tab.getId(), tab.getUrl().getSpec(), tab.getTitle(), true);
        }
        mTabModelObserver =
                new TabModelObserver() {
                    @Override
                    public void didAddTab(
                            Tab tab,
                            @TabLaunchType int type,
                            @TabCreationState int creationState,
                            boolean markedForSelection) {
                        if (tab.getId() == mMovingTab) {
                            // It keeps its entry (SpacesAndroid::TabOpened()).
                            mMovingTab = Tab.INVALID_TAB_ID;
                            mBridge.tabOpened(
                                    tab.getId(), tab.getUrl().getSpec(), tab.getTitle(), false);
                            TabModelUtils.selectTabById(
                                    mActivity.getTabModelSelector(),
                                    tab.getId(),
                                    TabSelectionType.FROM_USER);
                            return;
                        }
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

                    // Not tabRemoved(): the tab moved to another window, and keeps its entry.
                };
        mTabs.addObserver(mTabModelObserver);
        mTabObserver = new TabModelSelectorTabObserver(activity.getTabModelSelector()) {
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
        setUpDocking(parent);
    }

    /** Whether the sidebar is a permanent pane (wide windows). */
    public boolean isDocked() {
        return mDocked;
    }

    // On wide windows (tablets, unfolded foldables, desktop windows) the sidebar is a pane next to
    // the browser, as on the desktop: Chrome's coordinator moves over by its width.
    private void setUpDocking(ViewGroup coordinator) {
        if (!(coordinator.getParent() instanceof FrameLayout root)) return;
        mCoordinator = coordinator;
        mRoot = root;
        root.addOnLayoutChangeListener(
                (v, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
                    if (right - left != oldRight - oldLeft) v.post(this::updateDocking);
                });
        updateDocking();
    }

    private void updateDocking() {
        if (mCoordinator == null || mRoot == null || mRoot.getWidth() == 0) return;
        float density = mActivity.getResources().getDisplayMetrics().density;
        boolean dock = mRoot.getWidth() / density >= PANE_MIN_WINDOW_DP;
        if (dock != mDocked) {
            mDocked = dock;
            if (dock) onDismiss();
        }
        showPane(mDocked && !mPaneHidden);
    }

    private void showPane(boolean show) {
        assert mCoordinator != null && mRoot != null;
        boolean shown = mPane != null && mPane.getParent() == mRoot;
        if (show == shown) return;
        float density = mActivity.getResources().getDisplayMetrics().density;
        int paneWidth = Math.round(PANE_WIDTH_DP * density);
        if (mPane == null) {
            mPane = SidebarUi.createPaneView(mActivity, mActivity, mActivity, mState, this);
        }
        ViewGroup.MarginLayoutParams params =
                (ViewGroup.MarginLayoutParams) mCoordinator.getLayoutParams();
        if (show) {
            mRoot.addView(
                    mPane,
                    new FrameLayout.LayoutParams(
                            paneWidth, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.START));
            params.setMarginStart(paneWidth);
        } else {
            mRoot.removeView(mPane);
            params.setMarginStart(0);
        }
        mCoordinator.setLayoutParams(params);
    }

    /**
     * Ctrl+S, as on the desktop: puts the pane away or brings it back on wide windows, and opens or
     * closes the drawer on narrow ones.
     */
    public void toggleSidebar() {
        if (mDocked) {
            mPaneHidden = !mPaneHidden;
            showPane(!mPaneHidden);
        } else if (mState.getOpen()) {
            onDismiss();
        } else {
            showSidebar();
        }
    }

    /** The activity is going away: stops following its tabs. */
    public void destroy() {
        mTabs.removeObserver(mTabModelObserver);
        mTabObserver.destroy();
        mBridge.destroy();
    }

    /** Runs when the spaces change (the bar shows the active space's icon). */
    public void setOnChanged(Runnable onChanged) {
        mOnChanged = onChanged;
    }

    /** Runs when the sidebar's Sync row is picked. */
    public void setOnOpenSync(Runnable onOpenSync) {
        mOnOpenSync = onOpenSync;
    }

    public SidebarState state() {
        return mState;
    }

    /** The space this window shows (each window has its own), or empty before they load. */
    public String shownSpaceId() {
        return mBridge.shownSpace();
    }

    public void showSidebar() {
        if (mDocked) {
            // The pane is always there, unless Ctrl+S put it away.
            if (mPaneHidden) toggleSidebar();
            return;
        }
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

    /**
     * Moves a tab that's open in another window of the app here, as the desktop does when a node
     * whose tab lives in another window is activated (sidebar.md, "Move on focus"). Page state
     * survives: it's Chrome's own move between windows.
     */
    private boolean moveHere(int tabId) {
        if (!MultiWindowUtils.isMultiInstanceApi31Enabled()) return false;
        TabWindowInfo info = TabWindowManagerSingleton.getInstance().getTabWindowInfoById(tabId);
        if (info == null
                || info.type != TabWindowInfo.TabWindowType.TABBED
                || info.tab.isIncognito()
                || info.windowId == mActivity.getWindowId()) {
            return false;
        }
        mMovingTab = tabId;
        MultiInstanceOrchestratorFactory.getInstance()
                .moveTabsToWindowByIdChecked(
                        mActivity.getWindowId(),
                        List.of(info.tab),
                        TabList.INVALID_TAB_INDEX,
                        TabList.INVALID_TAB_INDEX,
                        /* bringToFront= */ true);
        return true;
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
        } else if (tabId != Tab.INVALID_TAB_ID && moveHere(tabId)) {
            // Selected when it arrives (didAddTab()).
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
        int tabId = mBridge.tabFor(nodeId);
        if (tabId != Tab.INVALID_TAB_ID && mTabs.getTabById(tabId) != null) {
            // Its tab stays open, as an ordinary tab in Today.
            mBridge.moveNode(nodeId, "", "today");
        } else {
            mBridge.removeNode(nodeId);
        }
    }

    @Override
    public void onMoveNode(String nodeId, String spaceId, String section) {
        mBridge.moveNode(nodeId, spaceId, section);
    }

    @Override
    public void onCopyLink(String url) {
        ClipboardManager clipboard =
                (ClipboardManager) mActivity.getSystemService(Context.CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(ClipData.newPlainText("URL", url));
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
    public void onEditSpace(String spaceId, String name, String emoji, String tint) {
        mBridge.editSpace(spaceId, name, emoji, tint);
    }

    @Override
    public void onRemoveSpace(String spaceId) {
        mBridge.removeSpace(spaceId);
    }

    @Override
    public void onNewTab() {
        onDismiss();
        mActivity.getTabCreator(false).launchNtp(TabLaunchType.FROM_CHROME_UI);
    }

    @Override
    public void onOpenSync() {
        onDismiss();
        if (mOnOpenSync != null) mOnOpenSync.run();
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
