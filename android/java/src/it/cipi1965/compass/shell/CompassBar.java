// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.view.KeyEvent;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.extras.CompassIntents;

import org.chromium.base.Callback;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ActivityTabProvider;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.compositor.CompositorViewHolder;
import org.chromium.chrome.browser.compositor.layouts.LayoutManagerChrome;
import org.chromium.chrome.browser.browser_controls.BottomControlsLayer;
import org.chromium.chrome.browser.browser_controls.BrowserControlsOffsetTagsInfo;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker.LayerScrollBehavior;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker.LayerType;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker.LayerVisibility;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.layouts.LayoutType;
import org.chromium.chrome.browser.lifecycle.DestroyObserver;
import org.chromium.chrome.browser.omnibox.LocationBarCoordinator;
import org.chromium.chrome.browser.preferences.Pref;
import org.chromium.chrome.browser.toolbar.ToolbarManager;
import org.chromium.chrome.browser.toolbar.bottom.ScrollingBottomViewSceneLayer;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.tab.TabSelectionType;
import org.chromium.chrome.browser.tabmodel.TabModelUtils;
import org.chromium.components.browser_ui.widget.ViewResourceFrameLayout;
import org.chromium.components.user_prefs.UserPrefs;
import org.chromium.url.GURL;

import java.util.Map;
import java.util.WeakHashMap;

/**
 * Compass's bottom bar (CompassBarUi.kt) in Chrome's tabbed activity. It sits in the bottom
 * controls stack, so web content makes room for it, and follows the activity's current tab.
 *
 * <p>Shell spike (milestone C1): Chrome's own toolbar stays for now.
 */
public class CompassBar implements BottomControlsLayer, CompassBarUi.Actions {
    // The floating capsule (54) and its margins.
    private static final int HEIGHT_DP = 62;

    private final ChromeTabbedActivity mActivity;
    private final BottomControlsStacker mBottomControls;
    private final CompassBarState mState = new CompassBarState();
    private final int mHeightPx;
    private @Nullable View mView;
    // The bar in a frame that the compositor can draw a copy of, which scrolls off with the page
    // (as Chrome's bottom toolbar does), and that copy; null if the compositor wasn't up.
    private @Nullable BarFrame mFrame;
    private @Nullable ScrollingBottomViewSceneLayer mSceneLayer;
    private @Nullable CommandBar mCommandBar;
    private @Nullable Spaces mSpaces;
    private @Nullable ClaudeChat mClaude;
    private @Nullable Integer mBackgroundColor;
    private @Nullable SyncScreen mSync;
    private @Nullable Extensions mExtensions;
    private @Nullable TabPicker mTabPicker;
    private @Nullable PageMenu mPageMenu;
    private @Nullable CompassSettings mSettings;
    private ActivityTabProvider.@Nullable ActivityTabTabObserver mTabObserver;
    private @Nullable TabModel mTabs;
    private final Callback<Integer> mTabCountObserver =
            count -> {
                if (mTabs != null) mState.setTabCount(mTabs.getCount());
            };

    // Each activity's bar, for the pages that need it (the start page).
    private static final Map<Activity, CompassBar> sBars = new WeakHashMap<>();

    /** The bar of |activity|, if it has one. */
    public static @Nullable CompassBar forActivity(Activity activity) {
        return sBars.get(activity);
    }

    public CompassBar(ChromeTabbedActivity activity, BottomControlsStacker bottomControls) {
        mActivity = activity;
        mBottomControls = bottomControls;
        mHeightPx = Math.round(HEIGHT_DP * activity.getResources().getDisplayMetrics().density);
    }

    public void attach() {
        sBars.put(mActivity, this);
        View view = CompassBarUi.createView(mActivity, mActivity, mActivity, mState, this);
        // Chrome's bottom layers (the chin, its bottom toolbar, the mini player) live at the
        // bottom of its coordinator, which the stack's offsets are relative to; Chrome pads it or
        // not for the system navigation bar.
        ViewGroup coordinator =
                mActivity.findViewById(
                        mActivity
                                .getResources()
                                .getIdentifier("coordinator", "id", mActivity.getPackageName()));
        CoordinatorLayout.LayoutParams params =
                new CoordinatorLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, mHeightPx);
        params.gravity = Gravity.START | Gravity.BOTTOM;
        BarFrame frame = new BarFrame(mActivity);
        frame.setId(View.generateViewId());
        frame.addView(
                view,
                new ViewGroup.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        coordinator.addView(frame, params);
        mFrame = frame;
        mView = view;
        attachCompositedCopy(frame);
        mCommandBar = new CommandBar(mActivity, coordinator);
        Spaces spaces = new Spaces(mActivity, coordinator);
        mSpaces = spaces;
        SyncScreen sync = new SyncScreen(mActivity, coordinator);
        mSync = sync;
        mClaude = new ClaudeChat(mActivity, coordinator);
        mExtensions = new Extensions(mActivity, coordinator, view);
        mTabPicker = new TabPicker(mActivity, coordinator, this::newTab);
        mPageMenu = new PageMenu(mActivity, coordinator, this);
        CompassSettings settings = new CompassSettings(mActivity, coordinator, this);
        mSettings = settings;
        Favicons.init(mActivity.getTabModelSelector().getModel(false).getProfile());
        spaces.setOnOpenSettings(settings::show);
        // Compass has no browser sign-in (as on the desktop): no sign-in promos either.
        UserPrefs.get(mActivity.getTabModelSelector().getModel(false).getProfile())
                .setBoolean(Pref.SIGNIN_ALLOWED, false);
        redirectOmniboxFocus();
        // Compass's own intents (the tile, the widget, shortcuts, the share target).
        handleIntent(mActivity.getIntent());
        spaces.setOnChanged(
                () -> {
                    SidebarState.Space space = spaces.state().getActiveSpace();
                    mState.setSpaceLabel(
                            space == null
                                    ? ""
                                    : !space.emoji.isEmpty()
                                            ? space.emoji
                                            : space.name.isEmpty()
                                                    ? ""
                                                    : space.name.substring(0, 1).toUpperCase());
                    mState.setSpaceTint(space == null ? "" : space.tint);
                });

        // Registers itself with the tab provider.
        mTabObserver =
                new ActivityTabProvider.ActivityTabTabObserver(
                        mActivity.getActivityTabProvider(), true) {
                    @Override
                    protected void onObservingDifferentTab(@Nullable Tab tab) {
                        update(tab);
                    }

                    @Override
                    public void onUrlUpdated(Tab tab) {
                        update(tab);
                    }

                    @Override
                    public void onTitleUpdated(Tab tab) {
                        update(tab);
                    }

                    @Override
                    public void onLoadStopped(Tab tab, boolean toDifferentDocument) {
                        update(tab);
                    }

                    @Override
                    public void onFaviconUpdated(
                            Tab tab, @Nullable Bitmap icon, @Nullable GURL iconUrl) {
                        Favicons.put(tab.getUrl().getSpec(), icon);
                    }
                };

        TabModel tabs = mActivity.getTabModelSelector().getModel(false);
        mTabs = tabs;
        tabs.getTabCountSupplier().addSyncObserverAndCall(mTabCountObserver);

        mBottomControls.addLayer(this);
        mBottomControls.requestLayerUpdate(false);
        mActivity.getLifecycleDispatcher().register((DestroyObserver) this::destroy);
    }

    // The activity is going away (a window closed, or Android recreating it): the native bridges
    // and the tab observers go with it.
    private void destroy() {
        sBars.remove(mActivity);
        if (mTabObserver != null) mTabObserver.destroy();
        if (mTabs != null) mTabs.getTabCountSupplier().removeObserver(mTabCountObserver);
        mBottomControls.removeLayer(this);
        if (mSpaces != null) mSpaces.destroy();
        if (mSync != null) mSync.destroy();
        if (mClaude != null) mClaude.destroy();
        if (mExtensions != null) mExtensions.destroy();
        mExtensions = null;
        if (mTabPicker != null) mTabPicker.destroy();
        mTabPicker = null;
        mPageMenu = null;
        mSettings = null;
        mTabObserver = null;
        mSpaces = null;
        mSync = null;
        mClaude = null;
        mCommandBar = null;
    }

    private void update(@Nullable Tab tab) {
        if (tab == null) {
            mState.setHost("");
            mState.setUrl("");
            mState.setCanGoBack(false);
            mState.setCanGoForward(false);
            return;
        }
        GURL url = tab.getUrl();
        // The new tab page (chrome-native://newtab) shows the bar's placeholder.
        String host = url.getScheme().startsWith("chrome") ? "" : url.getHost();
        mState.setHost(host);
        mState.setUrl(host.isEmpty() ? "" : url.getSpec());
        mState.setDesktopSite(
                tab.getWebContents() != null
                        && tab.getWebContents().getNavigationController().getUseDesktopUserAgent());
        mState.setCanGoBack(tab.canGoBack());
        mState.setCanGoForward(tab.canGoForward());
        mState.setIncognito(tab.isIncognito());
    }

    // Chrome still focuses its (hidden) location bar: from the new tab page's search box, or
    // Ctrl+L. The command bar takes over.
    private void redirectOmniboxFocus() {
        ToolbarManager toolbar = mActivity.getToolbarManager();
        if (toolbar == null) return;
        toolbar.getOmniboxStubSupplier()
                .onAvailable(
                        omnibox ->
                                omnibox.addUrlFocusChangeListener(
                                        hasFocus -> {
                                            if (!hasFocus || mCommandBar == null) return;
                                            if (toolbar.getLocationBar()
                                                    instanceof LocationBarCoordinator location) {
                                                location.clearOmniboxFocus();
                                            }
                                            // For the tab on screen (on the new tab page,
                                            // that's the new tab).
                                            mCommandBar.open(/* inNewTab= */ false);
                                        }));
    }

    /** Handles Compass's own intents (CompassIntents); returns whether |intent| was one. */
    public boolean handleIntent(@Nullable Intent intent) {
        if (intent == null || !CompassIntents.isCompassIntent(intent)) return false;
        String action = intent.getAction();
        if (CompassIntents.ACTION_COMMAND_BAR.equals(action)) {
            if (mCommandBar != null) mCommandBar.open(/* inNewTab= */ true);
        } else if (CompassIntents.ACTION_OPEN_SPACE.equals(action)) {
            String spaceId = intent.getStringExtra(CompassIntents.EXTRA_SPACE_ID);
            if (spaceId != null && mSpaces != null) mSpaces.openSpace(spaceId);
        } else if (CompassIntents.ACTION_PIN_URL.equals(action)) {
            String url = intent.getStringExtra(CompassIntents.EXTRA_URL);
            if (url != null && mSpaces != null) mSpaces.openAndPin(url);
        } else if (CompassIntents.ACTION_PAIR.equals(action)) {
            String server = intent.getStringExtra(CompassIntents.EXTRA_SERVER);
            String code = intent.getStringExtra(CompassIntents.EXTRA_CODE);
            if (server != null && code != null && mSync != null) mSync.join(server, code);
        }
        // Handled once, even if the activity is recreated with it.
        intent.setAction(null);
        return true;
    }

    /**
     * Compass's keyboard shortcuts, as on the desktop (Ctrl for Cmd); returns whether |event| was
     * one. The rest are Chrome's (KeyboardShortcuts).
     */
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (mExtensions != null && mExtensions.dispatchKeyEvent(event)) return true;
        if (!event.isCtrlPressed() || event.isAltPressed() || event.isMetaPressed()) return false;
        boolean shift = event.isShiftPressed();
        int key = event.getKeyCode();
        boolean ours =
                shift
                        ? key == KeyEvent.KEYCODE_E || key == KeyEvent.KEYCODE_C
                        : key == KeyEvent.KEYCODE_S
                                || key == KeyEvent.KEYCODE_T
                                || key == KeyEvent.KEYCODE_L
                                || key == KeyEvent.KEYCODE_K;
        if (!ours) return false;
        // Both halves of the key press are ours; the action runs on the first.
        if (event.getAction() != KeyEvent.ACTION_DOWN || event.getRepeatCount() != 0) return true;
        if (shift && key == KeyEvent.KEYCODE_E) {
            showClaude();
        } else if (shift) {
            copyUrl();
        } else if (key == KeyEvent.KEYCODE_S) {
            if (mSpaces != null) mSpaces.toggleSidebar();
        } else if (key == KeyEvent.KEYCODE_T) {
            newTab();
        } else {
            openAddress();
        }
        return true;
    }

    /** Shows the extensions sheet; false if the window has none. */
    public boolean showExtensions() {
        if (mExtensions == null) return false;
        mExtensions.show();
        return true;
    }

    /** Opens a sidebar entry: its tab, or a new one for it. */
    /** The space this window shows, or empty. */
    public String shownSpaceId() {
        return mSpaces != null ? mSpaces.shownSpaceId() : "";
    }

    public void openNode(String nodeId, String url) {
        if (mSpaces != null) mSpaces.onNodeClicked(nodeId, url);
    }

    void showSync() {
        if (mSync != null) mSync.show();
    }

    void showSettings() {
        if (mSettings != null) mSettings.show();
    }

    /** Sync's line for Settings. */
    String syncSummary() {
        return mSync != null ? mSync.summary() : "";
    }

    /** Runs one of Chrome's menu actions (its menu item's ID, by name). */
    void chromeAction(String idName) {
        int id = mActivity.getResources().getIdentifier(idName, "id", mActivity.getPackageName());
        if (id != 0) mActivity.getMenuOrKeyboardActionController().onMenuOrKeyboardAction(id, true);
    }

    /** The pinned entry or favorite showing |tabId|, or null. */
    SidebarState.@Nullable Node pinnedNodeFor(int tabId) {
        return mSpaces != null ? mSpaces.state().pinnedNodeFor(tabId) : null;
    }

    /** The window's space's name. */
    String spaceName() {
        SidebarState.Space space = mSpaces != null ? mSpaces.state().getActiveSpace() : null;
        return space != null ? space.name : "";
    }

    /** The space's entries with their pages, for the command bar: favorites, pinned, Today. */
    java.util.List<CommandBarState.Entry> sidebarEntries() {
        java.util.List<CommandBarState.Entry> entries = new java.util.ArrayList<>();
        if (mSpaces == null) return entries;
        SidebarState state = mSpaces.state();
        addEntries(entries, state.getFavorites(), "Favorites");
        addEntries(entries, state.getPinned(), "Pinned");
        addEntries(entries, state.getToday(), "Today");
        return entries;
    }

    private static void addEntries(
            java.util.List<CommandBarState.Entry> entries,
            java.util.List<SidebarState.Node> nodes,
            String where) {
        for (SidebarState.Node node : nodes) {
            if ("tab".equals(node.type) && !node.url.isEmpty()) {
                entries.add(new CommandBarState.Entry(node.id, node.title, node.url, where));
            }
            addEntries(entries, node.children, where);
        }
    }

    /** The window's space's tint ("#rrggbb", or empty). */
    String spaceTint() {
        return mState.getSpaceTint();
    }

    java.util.List<ExtensionsState.Action> extensionActions() {
        return mExtensions != null ? mExtensions.actions() : java.util.Collections.emptyList();
    }

    void runExtension(String id) {
        if (mExtensions != null) mExtensions.onRunAction(id);
    }

    /** Pins the tab on screen to the space, or unpins it (its tab stays open, in Today). */
    void togglePin() {
        Tab tab = mActivity.getActivityTab();
        if (tab == null || mSpaces == null) return;
        SidebarState.Node node = pinnedNodeFor(tab.getId());
        if (node != null) {
            mSpaces.onUnpinNode(node.id);
        } else {
            mSpaces.pinCurrentTab(false);
        }
    }

    @Override
    public void copyUrl() {
        Tab tab = mActivity.getActivityTab();
        if (tab == null) return;
        ClipboardManager clipboard =
                (ClipboardManager) mActivity.getSystemService(Context.CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(ClipData.newPlainText("URL", tab.getUrl().getSpec()));
    }

    // CompassBarUi.Actions:

    @Override
    public void showSidebar() {
        if (mSpaces != null) mSpaces.showSidebar();
    }

    @Override
    public void openAddress() {
        if (mCommandBar != null) mCommandBar.open(/* inNewTab= */ false);
    }

    @Override
    public void newTab() {
        if (mCommandBar != null) mCommandBar.open(/* inNewTab= */ true);
    }

    @Override
    public void switchTab(int offset) {
        TabModel model = mActivity.getTabModelSelector().getCurrentModel();
        int index = model.index() + offset;
        if (index < 0 || index >= model.getCount()) return;
        Tab tab = model.getTabAt(index);
        if (tab != null) {
            TabModelUtils.selectTabById(
                    mActivity.getTabModelSelector(), tab.getId(), TabSelectionType.FROM_USER);
        }
    }

    @Override
    public void showTabs() {
        if (mTabPicker != null) mTabPicker.show(mState.getSpaceTint());
    }

    /** Chrome's own tab switcher (tab groups). */
    public void showTabSwitcher() {
        mActivity.getLayoutManager().showLayout(LayoutType.HUB, /* animate= */ true);
    }

    public void showClaude() {
        if (mClaude != null) mClaude.show();
    }

    @Override
    public void showMenu() {
        if (mPageMenu != null) mPageMenu.show();
    }

    @Override
    public void newIncognitoTab() {
        mActivity
                .getTabCreator(true)
                .launchNtp(org.chromium.chrome.browser.tab.TabLaunchType.FROM_CHROME_UI);
    }

    @Override
    public void goBack() {
        Tab tab = mActivity.getActivityTab();
        if (tab != null && tab.canGoBack()) tab.goBack();
    }

    @Override
    public void goForward() {
        Tab tab = mActivity.getActivityTab();
        if (tab != null && tab.canGoForward()) tab.goForward();
    }

    @Override
    public void reload() {
        chromeAction("reload_menu_id");
    }

    @Override
    public void share() {
        chromeAction("share_menu_id");
    }

    @Override
    public void findInPage() {
        chromeAction("find_in_page_id");
    }

    @Override
    public void toggleDesktopSite() {
        chromeAction("request_desktop_site_id");
    }

    @Override
    public void backgroundColorChanged(int color) {
        if (mBackgroundColor != null && mBackgroundColor == color) return;
        mBackgroundColor = color;
        mBottomControls.requestLayerUpdate(false);
    }

    // BottomControlsLayer:

    @Override
    public @LayerType int getType() {
        return LayerType.COMPASS_BAR;
    }

    @Override
    public @LayerScrollBehavior int getScrollBehavior() {
        // Scrolling off needs the composited copy of the bar.
        return mSceneLayer != null
                ? LayerScrollBehavior.DEFAULT_SCROLL_OFF
                : LayerScrollBehavior.NEVER_SCROLL_OFF;
    }

    @Override
    public int getHeight() {
        return mHeightPx;
    }

    @Override
    public @LayerVisibility int getLayerVisibility() {
        return LayerVisibility.VISIBLE;
    }

    @Override
    public void onBrowserControlsOffsetUpdate(int layerYOffset) {
        if (mFrame == null) return;
        if (mSceneLayer != null) mSceneLayer.setYOffset(layerYOffset);
        // While the bar scrolls, the compositor draws its copy in step with the page; the view
        // comes back once it's all the way in (BottomControlsMediator does the same).
        boolean scrolled =
                mSceneLayer != null
                        && mActivity.getBrowserControlsManager().getBottomControlOffset() != 0;
        mFrame.setVisibility(scrolled ? View.INVISIBLE : View.VISIBLE);
        // Negative to move up; positive only mid-animation, as for Chrome's mini player.
        if (!scrolled) mFrame.setTranslationY(Math.min(0, layerYOffset));
    }

    @Override
    public int updateOffsetTag(BrowserControlsOffsetTagsInfo offsetTagsInfo) {
        // With browser controls in viz, viz moves the copy by this tag.
        if (mSceneLayer != null) {
            mSceneLayer.setOffsetTag(offsetTagsInfo.getBottomControlsOffsetTag());
        }
        return 0;
    }

    @Override
    public void clearOffsetTag() {
        if (mSceneLayer != null) mSceneLayer.setOffsetTag(null);
    }

    private void attachCompositedCopy(BarFrame frame) {
        CompositorViewHolder compositor = mActivity.getCompositorViewHolderSupplier().get();
        LayoutManagerChrome layouts = mActivity.getLayoutManager();
        if (compositor == null || layouts == null || compositor.getResourceManager() == null) {
            return;
        }
        compositor
                .getResourceManager()
                .getDynamicResourceLoader()
                .registerResource(frame.getId(), frame.getResourceAdapter());
        ScrollingBottomViewSceneLayer layer =
                new ScrollingBottomViewSceneLayer(frame, /* topShadowHeightPx= */ 0);
        layer.setShowShadow(false);
        layouts.addSceneOverlay(layer);
        mSceneLayer = layer;
    }

    /** A frame the compositor can copy (Chrome's ViewResourceFrameLayout, made in code). */
    private static class BarFrame extends ViewResourceFrameLayout {
        BarFrame(Context context) {
            super(context, null);
            // Inflation would make the resource adapter.
            onFinishInflate();
        }
    }

    @Override
    public @Nullable Integer getBackgroundColor() {
        return mBackgroundColor;
    }
}
