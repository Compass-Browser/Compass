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
import it.cipi1965.compass.extras.ListWidgetProvider;
import it.cipi1965.compass.extras.TodayWidgetProvider;
import it.cipi1965.compass.claude.ClaudeSidebarBridge;
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

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * The profile's spaces in the shell (docs/design/android.md, C2 and C3): keeps the native model
 * (SpacesBridge) in step with Chrome's regular tabs, and runs the sidebar (SidebarUi.kt).
 * Incognito tabs aren't in spaces.
 */
public class Spaces implements SidebarUi.Listener, PinPickerUi.Listener {
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
    private @Nullable Runnable mOnOpenSettings;
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
    // Pin to Compass: the tab the share target opened, until its pinned entry shows up; then the
    // picker for where it goes (PinPickerUi.kt).
    private int mPinningTab = Tab.INVALID_TAB_ID;
    private final PinPickerUi.State mPinPicker = new PinPickerUi.State();
    private @Nullable String mPinnedNode;
    private @Nullable View mPinView;
    // Claude in the sidebar (docs/design/ai-sidebar.md): the window's bridge, and the toast that
    // shows how Tidy Today went.
    private @Nullable ClaudeSidebarBridge mClaude;
    private @Nullable UndoToast mToast;
    private static final long SPARKLE_MS = 1500;
    // Tidy Today's toast in the sidebar: its Undo, and how long it stays.
    private @Nullable Runnable mToastUndo;
    private static final long TOAST_MS = 8000;
    private final Runnable mHideToast = this::hideToast;

    public Spaces(ChromeTabbedActivity activity, ViewGroup parent) {
        mActivity = activity;
        mTabs = activity.getTabModelSelector().getModel(false);
        mBridge =
                new SpacesBridge(mTabs.getProfile(), this::onModelChanged, this::closeArchivedTab);

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
                                tab.getId(),
                                tab.getUrl().getSpec(),
                                tab.getTitle(),
                                restored,
                                linkOpener(tab, type, markedForSelection));
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
                    public void tabClosureUndone(Tab tab) {
                        // Its Today entry went to the archive as it closed: it comes back out, at
                        // the top of Today (or a new entry, if there's none to bring back).
                        mBridge.tabClosureUndone(
                                tab.getId(), tab.getUrl().getSpec(), tab.getTitle());
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

    /**
     * The page a link opened in the background came from (the link's long-press "Open in new
     * tab", Ctrl+click, middle click), whose link folder it joins (docs/design/sidebar.md, "Link
     * folders"), or {@link Tab#INVALID_TAB_ID}. Links opened in the foreground, typed URLs and
     * other tabs open as before.
     */
    private static int linkOpener(
            Tab tab, @TabLaunchType int type, boolean markedForSelection) {
        if (markedForSelection) return Tab.INVALID_TAB_ID;
        if (type != TabLaunchType.FROM_LONGPRESS_BACKGROUND
                && type != TabLaunchType.FROM_LONGPRESS_BACKGROUND_IN_GROUP) {
            return Tab.INVALID_TAB_ID;
        }
        return tab.getParentId();
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

    /** Claude in the sidebar: Suggest a Name, Tidy Today and its toast (CompassBar sets them). */
    public void setClaudeSidebar(ClaudeSidebarBridge claude, UndoToast toast) {
        mClaude = claude;
        mToast = toast;
        refreshClaude();
    }

    /** What Claude offers in the sidebar now (ClaudeSidebarBridge's snapshot). */
    public void refreshClaude() {
        if (mClaude == null) return;
        try {
            JSONObject data = new JSONObject(mClaude.snapshot());
            mState.setSuggestNames(data.optBoolean("suggest_names"));
            mState.setTidyAvailable(data.optBoolean("tidy"));
            JSONArray busy = data.optJSONArray("busy");
            String shown = shownSpaceId();
            boolean tidying = false;
            for (int i = 0; busy != null && i < busy.length(); i++) {
                if (busy.optString(i).equals(shown)) tidying = true;
            }
            mState.setTidyBusy(tidying);
        } catch (JSONException e) {
            // Nothing offered.
        }
    }

    /** Claude just named |folderId|: it sparkles for a moment. */
    public void onFolderNamed(String folderId) {
        mState.setSparkleId(folderId);
        mView.postDelayed(
                () -> {
                    if (folderId.equals(mState.getSparkleId())) mState.setSparkleId("");
                },
                SPARKLE_MS);
    }

    /** Tidy Today of |spaceId| made |folders| folders: the toast, with Undo, if it's shown here. */
    public void onTidied(String spaceId, int undoId, int folders) {
        if (mToast == null || mClaude == null || !spaceId.equals(shownSpaceId())) return;
        ClaudeSidebarBridge claude = mClaude;
        showToast(
                folders == 1 ? "Tidied into 1 folder" : "Tidied into " + folders + " folders",
                () -> claude.undoTidy(undoId));
    }

    // The sidebar on a phone is a sheet in a window of its own, over the window's toast: while
    // it's open, the toast is in it.
    private void showToast(String text, @Nullable Runnable undo) {
        if (mState.getOpen() && !mDocked) {
            mToastUndo = undo;
            mState.setToast(text);
            mState.setToastUndoable(undo != null);
            mView.removeCallbacks(mHideToast);
            mView.postDelayed(mHideToast, TOAST_MS);
        } else if (mToast != null) {
            mToast.showMessage(text, undo);
        }
    }

    private void hideToast() {
        mView.removeCallbacks(mHideToast);
        mToastUndo = null;
        mState.setToast("");
    }

    @Override
    public void onToastUndo() {
        Runnable undo = mToastUndo;
        hideToast();
        if (undo != null) undo.run();
    }

    /** Tidy Today of |spaceId| didn't work: why, without Undo. */
    public void onTidyFailed(String spaceId, String message) {
        if (!spaceId.equals(shownSpaceId())) return;
        showToast(message, null);
    }

    /** Runs when the spaces change (the bar shows the active space's icon). */
    public void setOnChanged(Runnable onChanged) {
        mOnChanged = onChanged;
    }

    /** Runs when the sidebar's Sync row is picked. */
    public void setOnOpenSettings(Runnable onOpenSettings) {
        mOnOpenSettings = onOpenSettings;
    }

    public SidebarState state() {
        return mState;
    }

    /** The space this window shows (each window has its own), or empty before they load. */
    /** The chain's profiles and their spaces (Settings › Profiles), in the switcher's order. */
    public java.util.List<CompassSettingsUi.ProfileRow> profileRows() {
        java.util.LinkedHashMap<String, int[]> counts = new java.util.LinkedHashMap<>();
        java.util.Map<String, String> names = new java.util.HashMap<>();
        String own = "";
        for (SidebarState.Space space : mState.getAllSpaces()) {
            String key = space.profileDir.isEmpty() ? space.profile : space.profileDir;
            counts.computeIfAbsent(key, k -> new int[1])[0]++;
            names.put(key, space.profile);
            if (space.own) own = key;
        }
        java.util.List<CompassSettingsUi.ProfileRow> rows = new java.util.ArrayList<>();
        for (java.util.Map.Entry<String, int[]> entry : counts.entrySet()) {
            rows.add(
                    new CompassSettingsUi.ProfileRow(
                            names.get(entry.getKey()),
                            entry.getValue()[0],
                            entry.getKey().equals(own)));
        }
        return rows;
    }

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
        if (tab == null) return;
        mPinningTab = tab.getId();
        mBridge.pin(tab.getId(), false);
        showPinPicker();
    }

    /**
     * The Today widget's entry: its tab, if the entry is still in the shown space's Today; else
     * its page in a new tab.
     */
    public void openEntry(String nodeId, String url) {
        boolean inToday = false;
        for (SidebarState.Node node : mState.getToday()) {
            if (node.id.equals(nodeId)) inToday = true;
        }
        if (inToday) {
            onNodeClicked(nodeId, url);
            return;
        }
        GURL gurl = new GURL(url);
        if (!gurl.isValid()) return;
        mActivity
                .getTabCreator(false)
                .createNewTab(
                        new LoadUrlParams(gurl.getSpec(), PageTransition.AUTO_BOOKMARK),
                        TabLaunchType.FROM_CHROME_UI,
                        null);
    }

    // The Today widget (TodayWidgetProvider): the shown space's Today entries, as they change.
    private void publishToday() {
        if (!mState.getLoaded()) return;
        try {
            JSONArray rows = new JSONArray();
            for (SidebarState.Node node : mState.getToday()) {
                if (node.isFolder() || node.url.isEmpty()) continue;
                rows.put(
                        new JSONObject()
                                .put("id", node.id)
                                .put("title", node.title)
                                .put("url", node.url));
                if (rows.length() == ListWidgetProvider.rows()) break;
            }
            SidebarState.Space space = mState.getActiveSpace();
            String title = "Today";
            if (space != null && !space.name.isEmpty()) {
                title = (space.emoji.isEmpty() ? "" : space.emoji + " ") + space.name + " · Today";
            }
            ListWidgetProvider.publish(
                    mActivity,
                    new TodayWidgetProvider(),
                    new JSONObject().put("title", title).put("rows", rows).toString());
        } catch (JSONException e) {
            // Not published.
        }
    }

    // Once the shared page's entry is in the model: the picker, to move it elsewhere.
    private void showPinPicker() {
        if (mPinningTab == Tab.INVALID_TAB_ID) return;
        SidebarState.Node node = mState.pinnedNodeFor(mPinningTab);
        if (node == null) return;
        mPinningTab = Tab.INVALID_TAB_ID;
        mPinnedNode = node.id;
        mPinPicker.setTitle(node.title.isEmpty() ? node.url : node.title);
        mPinPicker.setParentId(mState.getPinnedId());
        if (mPinView == null) {
            mPinView =
                    PinPickerUi.createView(
                            mActivity, mActivity, mActivity, mState, mPinPicker, this);
            ((ViewGroup) mView.getParent())
                    .addView(
                            mPinView,
                            new CoordinatorLayout.LayoutParams(
                                    ViewGroup.LayoutParams.MATCH_PARENT,
                                    ViewGroup.LayoutParams.MATCH_PARENT));
        }
        mPinView.setVisibility(View.VISIBLE);
        mPinView.bringToFront();
        mPinPicker.setOpen(true);
    }

    @Override
    public void onPinInto(String parentId) {
        if (mPinnedNode == null || parentId.isEmpty()) return;
        if (mBridge.moveNodeTo(mPinnedNode, parentId, "")) mPinPicker.setParentId(parentId);
    }

    @Override
    public void onPinPickerDone() {
        mPinPicker.setOpen(false);
        mPinnedNode = null;
        // After the sheet has slid away.
        if (mPinView != null) {
            View view = mPinView;
            view.postDelayed(
                    () -> {
                        if (!mPinPicker.getOpen()) view.setVisibility(View.GONE);
                    },
                    400);
        }
    }

    private void onModelChanged() {
        mState.update(mBridge.snapshot(), currentTabId());
        // The shown space may have changed: whether its tidy waits.
        refreshClaude();
        showPinPicker();
        publishToday();
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

    // The archiver archived this tab's Today entry (it went unseen for the space's delay): the tab
    // closes, without undo; the entry is in the archive.
    private void closeArchivedTab(int tabId) {
        Tab tab = mTabs.getTabById(tabId);
        if (tab == null) return;
        mTabs.getTabRemover()
                .closeTabs(
                        org.chromium.chrome.browser.tabmodel.TabClosureParams.closeTab(tab)
                                .allowUndo(false)
                                .build(),
                        /* allowDialog= */ false);
    }

    // Closes an entry's open tab, with undo (UndoToast); returns false if it has none here.
    private boolean closeTabOf(String nodeId) {
        int tabId = mBridge.tabFor(nodeId);
        Tab tab = tabId != Tab.INVALID_TAB_ID ? mTabs.getTabById(tabId) : null;
        if (tab == null) return false;
        mTabs.getTabRemover()
                .closeTabs(
                        org.chromium.chrome.browser.tabmodel.TabClosureParams.closeTab(tab)
                                .allowUndo(true)
                                .build(),
                        /* allowDialog= */ false);
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
        // Closing a Today entry's tab sends the entry to the archive (SpacesAndroid::TabClosed());
        // undo is the shell's toast (UndoToast). One with no tab goes there directly.
        if (!closeTabOf(nodeId)) mBridge.archiveNode(nodeId);
    }

    @Override
    public void onArchiveNode(String nodeId) {
        onCloseNode(nodeId);
    }

    @Override
    public void onRestoreArchived(String entryId, String url) {
        String nodeId = mBridge.restoreArchived(entryId);
        if (!nodeId.isEmpty()) onNodeClicked(nodeId, url);
    }

    @Override
    public void onRemoveArchived(String entryId) {
        mBridge.removeArchived(entryId);
    }

    @Override
    public void onClearArchive() {
        mBridge.clearArchive();
    }

    @Override
    public void onSetArchiveAfter(int hours) {
        mBridge.setArchiveAfter(hours);
    }

    @Override
    public void onUseAsSavedUrl(String nodeId) {
        mBridge.useCurrentPageAsSavedUrl(nodeId);
    }

    @Override
    public void onBackToSavedUrl(String nodeId) {
        String url = mBridge.backToSavedUrl(nodeId);
        if (url.isEmpty()) return;
        int tabId = mBridge.tabFor(nodeId);
        Tab tab = tabId != Tab.INVALID_TAB_ID ? mTabs.getTabById(tabId) : null;
        // Its entry follows the tab (onUrlUpdated()).
        if (tab != null) tab.loadUrl(new LoadUrlParams(url, PageTransition.AUTO_BOOKMARK));
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
    public void onMoveNodeTo(String nodeId, String parentId, String beforeId) {
        mBridge.moveNodeTo(nodeId, parentId, beforeId);
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
        // A space of another profile shows in that profile's window, in place of this one
        // (docs/design/android.md, "Profiles").
        for (SidebarState.Space space : mState.getAllSpaces()) {
            if (space.id.equals(spaceId) && !space.own && !space.profileDir.isEmpty()) {
                onDismiss();
                it.cipi1965.compass.profiles.WindowProfiles.show(
                        mActivity, space.profileDir, spaceId);
                return;
            }
        }
        mBridge.setActiveSpace(spaceId);
    }

    @Override
    public void onAddSpace(String name) {
        mBridge.addSpace(name);
    }

    @Override
    public void onEditSpace(String spaceId, String name, String emoji) {
        mBridge.editSpace(spaceId, name, emoji);
    }

    @Override
    public void onSetSpaceTheme(String spaceId, SpaceTheme theme) {
        mBridge.setSpaceTheme(
                spaceId, theme.colorsCsv(), theme.intensity, theme.grain, theme.appearance.ordinal());
    }

    @Override
    public void onMoveSpaceToProfile(String spaceId, String profileDir) {
        // As the desktop's MoveSpaceToProfile(): the entries move, the tabs (which can't change
        // profile) close, and the space shows in a window of its new profile.
        int[] tabs = mBridge.moveSpaceToProfile(spaceId, profileDir);
        for (int tabId : tabs) closeArchivedTab(tabId);
        onDismiss();
        it.cipi1965.compass.profiles.WindowProfiles.show(mActivity, profileDir, spaceId);
    }

    @Override
    public void onRemoveSpace(String spaceId) {
        mBridge.removeSpace(spaceId);
    }

    @Override
    public void onNewTab() {
        onDismiss();
        // The command bar, for a new tab (as on iOS).
        CompassBar bar = CompassBar.forActivity(mActivity);
        if (bar != null) {
            bar.newTab();
        } else {
            mActivity.getTabCreator(false).launchNtp(TabLaunchType.FROM_CHROME_UI);
        }
    }

    @Override
    public void onNewIncognitoTab() {
        onDismiss();
        mActivity.getTabCreator(true).launchNtp(TabLaunchType.FROM_CHROME_UI);
    }

    @Override
    public void onClearToday() {
        // Every Today entry goes to the archive: its tab closes, and the entry with it; one step to
        // undo.
        List<Tab> tabs = new ArrayList<>();
        clearEntries(mState.getToday(), tabs);
        if (tabs.isEmpty()) return;
        mTabs.getTabRemover()
                .closeTabs(
                        org.chromium.chrome.browser.tabmodel.TabClosureParams.closeTabs(tabs)
                                .allowUndo(true)
                                .build(),
                        /* allowDialog= */ false);
    }

    // Today's entries, in its folders too (a folder closes with its last entry): those with a tab
    // here go in |tabs|, to close; the others go to the archive now.
    private void clearEntries(List<SidebarState.Node> nodes, List<Tab> tabs) {
        for (SidebarState.Node node : nodes) {
            if (node.isFolder()) {
                clearEntries(node.children, tabs);
                continue;
            }
            int tabId = mBridge.tabFor(node.id);
            Tab tab = tabId != Tab.INVALID_TAB_ID ? mTabs.getTabById(tabId) : null;
            if (tab != null) {
                tabs.add(tab);
            } else {
                mBridge.archiveNode(node.id);
            }
        }
    }

    @Override
    public void onRenameFolder(String folderId, String name) {
        mBridge.renameFolder(folderId, name);
    }

    @Override
    public void onUndoName(String folderId) {
        mBridge.undoFolderName(folderId);
    }

    @Override
    public void onSuggestName(String folderId) {
        if (mClaude != null) mClaude.suggestName(folderId);
    }

    @Override
    public void onTidyToday() {
        if (mClaude != null) mClaude.tidy(shownSpaceId());
    }

    @Override
    public void onOpenSettings() {
        onDismiss();
        if (mOnOpenSettings != null) mOnOpenSettings.run();
    }

    @Override
    public void onDismiss() {
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
