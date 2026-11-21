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
import android.widget.FrameLayout;

import androidx.coordinatorlayout.widget.CoordinatorLayout;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

import it.cipi1965.compass.claude.ClaudeSidebarBridge;
import it.cipi1965.compass.extras.CompassIntents;

import org.chromium.base.Callback;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ActivityTabProvider;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.cc.mojom.RootScrollOffsetUpdateFrequency;
import org.chromium.chrome.browser.browser_controls.BottomControlsLayer;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker.LayerScrollBehavior;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker.LayerType;
import org.chromium.chrome.browser.browser_controls.BottomControlsStacker.LayerVisibility;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.layouts.LayoutType;
import org.chromium.chrome.browser.lifecycle.DestroyObserver;
import org.chromium.chrome.browser.lifecycle.PauseResumeWithNativeObserver;
import org.chromium.chrome.browser.omnibox.LocationBarCoordinator;
import org.chromium.chrome.browser.preferences.Pref;
import org.chromium.chrome.browser.toolbar.ToolbarManager;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.tabmodel.TabModelSelectorTabModelObserver;
import org.chromium.chrome.browser.tab.TabSelectionType;
import org.chromium.chrome.browser.tabmodel.TabModelUtils;
import org.chromium.components.browser_ui.display_cutout.DisplayCutoutController;
import org.chromium.components.user_prefs.UserPrefs;
import org.chromium.content_public.browser.GestureListenerManager;
import org.chromium.content_public.browser.GestureStateListener;
import org.chromium.content_public.browser.NavigationHandle;
import org.chromium.content_public.browser.WebContents;
import org.chromium.url.GURL;
import org.chromium.url.Origin;

import java.util.Map;
import java.util.WeakHashMap;

/**
 * Compass's bottom bar (CompassBarUi.kt) in Chrome's tabbed activity, which follows the activity's
 * current tab. It floats over the page, as on iOS: it isn't one of Chrome's bottom controls (which
 * the page would end above), and it slides away as the page scrolls down and back as it scrolls up
 * (docs/design/android.md, "The bar").
 */
public class CompassBar implements CompassBarUi.Actions {
    private final BottomControlsStacker mBottomControls;
    // The bar's color, for the strip Chrome paints under the system's navigation bar (its bottom
    // chin), which takes the stack's color.
    private @Nullable Integer mBackgroundColor;
    private int mSplitHeight;
    private @Nullable SplitView mSplit;
    private final BottomControlsLayer mColorLayer =
            new BottomControlsLayer() {
                @Override
                public @LayerType int getType() {
                    return LayerType.COMPASS_BAR;
                }

                @Override
                public @LayerScrollBehavior int getScrollBehavior() {
                    return LayerScrollBehavior.NEVER_SCROLL_OFF;
                }

                // No height, the page runs under the bar; a split's bottom pane's when one shows.
                @Override
                public int getHeight() {
                    return mSplitHeight;
                }

                @Override
                public @LayerVisibility int getLayerVisibility() {
                    return LayerVisibility.VISIBLE;
                }

                @Override
                public void onBrowserControlsOffsetUpdate(int layerYOffset) {}

                @Override
                public @Nullable Integer getBackgroundColor() {
                    return mBackgroundColor;
                }
            };

    // The floating capsule (54) and its margins.
    private static final int HEIGHT_DP = 62;

    /** The window's space's theme, for the tab picker. */
    private SpaceTheme mSpaceTheme = SpaceTheme.GREY;

    private final ChromeTabbedActivity mActivity;
    private final CompassBarState mState = new CompassBarState();
    private final int mHeightPx;
    private @Nullable View mView;
    // The bar's frame, which slides away while the page scrolls down.
    private @Nullable FrameLayout mFrame;
    private boolean mBarHidden;
    // The page the bar follows the scrolling of.
    private @Nullable WebContents mScrollContents;
    private final GestureStateListener mScrollListener =
            new GestureStateListener() {
                @Override
                public void onVerticalScrollDirectionChanged(
                        boolean directionUp, float currentScrollRatio) {
                    setBarHidden(!directionUp);
                }

                // Also at the start of each scroll: a page Peek handed to a tab hears no direction
                // changes (its GestureListenerManager listens to the card's view delegate, which
                // the tab replaced). Not at a fling's start, whose direction Chromium reads from
                // the scroll-begin fields a fling doesn't fill (gesture_listener_manager.cc).
                @Override
                public void onScrollStarted(
                        int scrollOffsetY, int scrollExtentY, boolean isDirectionUp) {
                    setBarHidden(!isDirectionUp);
                }

                @Override
                public void onScrollOffsetOrExtentChanged(int scrollOffsetY, int scrollExtentY) {
                    // Back at the top of the page.
                    if (scrollOffsetY <= 0) setBarHidden(false);
                }
            };
    private @Nullable CommandBar mCommandBar;
    private @Nullable Spaces mSpaces;
    private @Nullable ClaudeChat mClaude;
    private @Nullable SyncScreen mSync;
    // Sharing with other people: the share sheet and the join screen.
    private @Nullable ShareSheet mShare;
    private @Nullable DevicesWidget mDevicesWidget;
    private @Nullable HistoryScreen mHistory;
    private @Nullable DownloadsScreen mDownloads;
    private @Nullable Extensions mExtensions;
    private @Nullable TabPicker mTabPicker;
    private @Nullable PageMenu mPageMenu;
    private @Nullable CompassSettings mSettings;
    private @Nullable FindBar mFindBar;
    private @Nullable UndoToast mUndoToast;
    // Peek's card, also the little window's (docs/design/android.md, "Peek", "Little windows").
    private @Nullable Peek mPeek;
    private @Nullable LinkMenu mLinkMenu;
    private @Nullable SiteSheet mSiteSheet;
    // Reader mode (docs/design/android.md, "Reader mode").
    private @Nullable Reader mReader;
    private @Nullable TabModelSelectorTabModelObserver mReaderTabsObserver;
    // Claude in the sidebar and the command bar (docs/design/ai-sidebar.md), on the regular profile.
    private @Nullable ClaudeSidebarBridge mClaudeSidebar;
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

    /**
     * |bottomControls|: Chrome's stack, where the bar has a layer of no height only for its color
     * (it floats over the page).
     */
    public CompassBar(ChromeTabbedActivity activity, BottomControlsStacker bottomControls) {
        mActivity = activity;
        mBottomControls = bottomControls;
        mHeightPx = Math.round(HEIGHT_DP * activity.getResources().getDisplayMetrics().density);
    }

    public void attach() {
        sBars.put(mActivity, this);
        // Tab-modal dialogs (permission prompts, alerts) dock above the bar, as its sheets
        // (docs/design/android.md, "Dialogs").
        org.chromium.components.browser_ui.modaldialog.TabModalPresenter.setCompassBottomDock(
                CompassBar::aboveBar);
        View view = CompassBarUi.createView(mActivity, mActivity, mActivity, mState, this);
        // At the bottom of Chrome's coordinator, over the page, which runs under it.
        ViewGroup coordinator =
                mActivity.findViewById(
                        mActivity
                                .getResources()
                                .getIdentifier("coordinator", "id", mActivity.getPackageName()));
        CoordinatorLayout.LayoutParams params =
                new CoordinatorLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, mHeightPx);
        params.gravity = Gravity.START | Gravity.BOTTOM;
        FrameLayout frame = new FrameLayout(mActivity);
        frame.addView(
                view,
                new ViewGroup.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        coordinator.addView(frame, params);
        mFrame = frame;
        // Clear of the system's navigation bar: Chrome's views take the window insets before the
        // bar would see them, so it reads the window's own as the coordinator lays out.
        coordinator.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> keepClearOfNavBar());
        keepClearOfNavBar();
        mView = view;
        mCommandBar = new CommandBar(mActivity, coordinator);
        Spaces spaces = new Spaces(mActivity, coordinator);
        mSpaces = spaces;
        SyncScreen sync = new SyncScreen(mActivity, coordinator);
        mSync = sync;
        ShareSheet share = new ShareSheet(mActivity, coordinator, spaces::refreshRooms);
        mShare = share;
        spaces.setShareSheet(share);
        mClaude =
                new ClaudeChat(
                        mActivity,
                        coordinator,
                        spaces,
                        mHeightPx
                                + Math.round(
                                        10 * mActivity.getResources().getDisplayMetrics().density));
        mHistory = new HistoryScreen(mActivity, coordinator);
        mDownloads = new DownloadsScreen(mActivity, coordinator);
        mDevicesWidget =
                new DevicesWidget(
                        mActivity, mActivity.getTabModelSelector().getModel(false).getProfile());
        mExtensions = new Extensions(mActivity, coordinator, view);
        mTabPicker = new TabPicker(mActivity, coordinator, this::newTab);
        mPageMenu = new PageMenu(mActivity, coordinator, this);
        CompassSettings settings = new CompassSettings(mActivity, coordinator, this);
        mSettings = settings;
        mFindBar = new FindBar(mActivity, coordinator);
        mPeek = new Peek(mActivity, coordinator, spaces);
        mLinkMenu = new LinkMenu(mActivity, coordinator, spaces, mPeek);
        mSiteSheet = new SiteSheet(mActivity, coordinator);
        mSplit =
                new SplitView(
                        mActivity,
                        coordinator,
                        frame,
                        spaces,
                        height -> {
                            if (mSplitHeight == height) return;
                            mSplitHeight = height;
                            mBottomControls.requestLayerUpdate(false);
                        });
        Reader reader =
                new Reader(
                        mActivity,
                        coordinator,
                        frame,
                        (readerable, showing) -> {
                            mState.setReaderable(readerable);
                            mState.setInReader(showing);
                        });
        mReader = reader;
        mReaderTabsObserver =
                new TabModelSelectorTabModelObserver(mActivity.getTabModelSelector()) {
                    @Override
                    public void willCloseTab(Tab tab, boolean didCloseAlone) {
                        reader.onTabClosed(tab.getId());
                    }
                };
        // Clear of the bar, as iOS's toast over its bar.
        mUndoToast =
                new UndoToast(
                        mActivity,
                        coordinator,
                        mHeightPx
                                + Math.round(
                                        10 * mActivity.getResources().getDisplayMetrics().density));
        Favicons.init(mActivity.getTabModelSelector().getModel(false).getProfile());
        CommandBar commandBar = mCommandBar;
        ClaudeSidebarBridge claudeSidebar =
                new ClaudeSidebarBridge(
                        mActivity.getTabModelSelector().getModel(false).getProfile(),
                        new ClaudeSidebarBridge.Listener() {
                            @Override
                            public void onChanged() {
                                spaces.refreshClaude();
                            }

                            @Override
                            public void onFolderNamed(String folderId) {
                                spaces.onFolderNamed(folderId);
                            }

                            @Override
                            public void onTidied(String spaceId, int undoId, int folders) {
                                spaces.onTidied(spaceId, undoId, folders);
                            }

                            @Override
                            public void onTidyFailed(String spaceId, String message) {
                                spaces.onTidyFailed(spaceId, message);
                            }

                            @Override
                            public void onAnswer(String text) {
                                commandBar.onAnswer(text);
                            }

                            @Override
                            public void onAnswerDone(String text, String error) {
                                commandBar.onAnswerDone(text, error);
                            }
                        });
        mClaudeSidebar = claudeSidebar;
        spaces.setClaudeSidebar(claudeSidebar, mUndoToast);
        commandBar.setClaudeSidebar(claudeSidebar);
        spaces.setOnOpenSettings(settings::show);
        // Compass has no browser sign-in (as on the desktop): no sign-in promos either.
        UserPrefs.get(mActivity.getTabModelSelector().getModel(false).getProfile())
                .setBoolean(Pref.SIGNIN_ALLOWED, false);
        redirectOmniboxFocus();
        // Compass's own intents (the tile, the widget, shortcuts, the share target).
        boolean handled = handleIntent(mActivity.getIntent());
        // Settings → On Launch: the sidebar, or the tab last shown.
        OnLaunch.run(mActivity, spaces, handled);
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
                    mSpaceTheme = space == null ? SpaceTheme.GREY : space.theme;
                    if (mSplit != null) {
                        mSplit.setAccent(mSpaceTheme.getAccentArgb());
                        mSplit.postUpdate();
                    }
                });

        // Registers itself with the tab provider.
        mTabObserver =
                new ActivityTabProvider.ActivityTabTabObserver(
                        mActivity.getActivityTabProvider(), true) {
                    @Override
                    protected void onObservingDifferentTab(@Nullable Tab tab) {
                        update(tab);
                        watchScroll(tab);
                        // Another tab shown closes Peek, as on iOS.
                        if (mPeek != null) mPeek.onTabShown(tab);
                        if (mReader != null) mReader.onTabShown(tab);
                        if (mSplit != null) mSplit.postUpdate();
                    }

                    @Override
                    public void onUrlUpdated(Tab tab) {
                        update(tab);
                        // Another page: the bar comes back.
                        setBarHidden(false);
                    }

                    @Override
                    public void onContentChanged(Tab tab) {
                        watchScroll(tab);
                    }

                    @Override
                    public void onPageLoadFinished(Tab tab, GURL url) {
                        if (mReader != null) mReader.onPageLoaded(tab);
                    }

                    @Override
                    public void onDidFinishNavigationInPrimaryMainFrame(
                            Tab tab, NavigationHandle navigation) {
                        if (mReader != null) mReader.onNavigated(tab, navigation);
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

        mBottomControls.addLayer(mColorLayer);
        mBottomControls.requestLayerUpdate(false);
        mActivity.addOnUserLeaveHintListener(mLeaveHint);
        mActivity.getLifecycleDispatcher().register(mPipResume);
        mActivity.getLifecycleDispatcher().register((DestroyObserver) this::destroy);
    }

    // Auto picture-in-picture when Compass is left for another app (docs/design/android.md, "Auto
    // picture-in-picture"): the shown tab's playing video goes into picture-in-picture, as the
    // desktop's does when its window is covered, and comes back with the app. A fullscreen video
    // is Chrome's own (FullscreenVideoPictureInPictureController, which turns the whole window).
    private @Nullable WebContents mPipLeftContents;
    private final Runnable mLeaveHint = this::onLeavingApp;
    private final PauseResumeWithNativeObserver mPipResume =
            new PauseResumeWithNativeObserver() {
                @Override
                public void onResumeWithNative() {
                    WebContents left = mPipLeftContents;
                    mPipLeftContents = null;
                    if (left != null && !left.isDestroyed()) PipBridge.setWindowLeft(left, false);
                }

                @Override
                public void onPauseWithNative() {}
            };

    private void onLeavingApp() {
        Tab tab = mActivity.getActivityTab();
        if (tab == null || tab.getWebContents() == null) return;
        if (mActivity.getFullscreenManager().getPersistentFullscreenMode()) return;
        mPipLeftContents = tab.getWebContents();
        PipBridge.setWindowLeft(tab.getWebContents(), true);
    }

    // The activity is going away (a window closed, or Android recreating it): the native bridges
    // and the tab observers go with it.
    private void destroy() {
        sBars.remove(mActivity);
        if (mTabObserver != null) mTabObserver.destroy();
        if (mTabs != null) mTabs.getTabCountSupplier().removeObserver(mTabCountObserver);
        watchScroll(null);
        mBottomControls.removeLayer(mColorLayer);
        mActivity.removeOnUserLeaveHintListener(mLeaveHint);
        mActivity.getLifecycleDispatcher().unregister(mPipResume);
        if (mSpaces != null) mSpaces.destroy();
        if (mSync != null) mSync.destroy();
        if (mSpaces != null) mSpaces.setShareSheet(null);
        if (mShare != null) mShare.destroy();
        mShare = null;
        if (mDevicesWidget != null) mDevicesWidget.destroy();
        mDevicesWidget = null;
        if (mHistory != null) mHistory.destroy();
        mHistory = null;
        if (mDownloads != null) mDownloads.destroy();
        mDownloads = null;
        if (mClaude != null) mClaude.destroy();
        if (mClaudeSidebar != null) mClaudeSidebar.destroy();
        mClaudeSidebar = null;
        if (mExtensions != null) mExtensions.destroy();
        mExtensions = null;
        if (mTabPicker != null) mTabPicker.destroy();
        mTabPicker = null;
        mPageMenu = null;
        mSettings = null;
        if (mFindBar != null) mFindBar.onDone();
        mFindBar = null;
        if (mUndoToast != null) mUndoToast.destroy();
        mUndoToast = null;
        if (mLinkMenu != null) mLinkMenu.destroy();
        mLinkMenu = null;
        if (mSiteSheet != null) mSiteSheet.destroy();
        mSiteSheet = null;
        if (mPeek != null) mPeek.destroy();
        mPeek = null;
        if (mSplit != null) mSplit.destroy();
        mSplit = null;
        if (mReaderTabsObserver != null) mReaderTabsObserver.destroy();
        mReaderTabsObserver = null;
        if (mReader != null) mReader.destroy();
        mReader = null;
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
        } else if (CompassIntents.ACTION_INCOGNITO.equals(action)) {
            newIncognitoTab();
        } else if (CompassIntents.ACTION_CLAUDE.equals(action)) {
            showClaude();
        } else if (CompassIntents.ACTION_SIDEBAR.equals(action)) {
            showSidebar();
        } else if (CompassIntents.ACTION_OPEN_ENTRY.equals(action)) {
            String nodeId = intent.getStringExtra(CompassIntents.EXTRA_NODE_ID);
            String url = intent.getStringExtra(CompassIntents.EXTRA_URL);
            if (url != null && mSpaces != null) {
                String spaceId = intent.getStringExtra(CompassIntents.EXTRA_SPACE_ID);
                mSpaces.openEntry(
                        nodeId != null ? nodeId : "", url, spaceId != null ? spaceId : "");
            }
        } else if (CompassIntents.ACTION_PAIR.equals(action)) {
            String server = intent.getStringExtra(CompassIntents.EXTRA_SERVER);
            String code = intent.getStringExtra(CompassIntents.EXTRA_CODE);
            if (server != null && code != null && mSync != null) mSync.join(server, code);
        } else if (CompassIntents.ACTION_JOIN_ROOM.equals(action)) {
            // An invite link to something shared (docs/design/sync.md, "Rooms: clients").
            String link = intent.getStringExtra(CompassIntents.EXTRA_URL);
            if (link != null && mShare != null) {
                // The join screen goes over everything, the sidebar too.
                if (mSpaces != null) mSpaces.onDismiss();
                mShare.join(link);
            }
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
        if (mPeek != null && mPeek.dispatchKeyEvent(event)) return true;
        // Reader: Shift+Alt+R, the desktop's keys on Windows and Linux; Esc goes back to the page.
        if (mReader != null
                && ((event.getKeyCode() == KeyEvent.KEYCODE_R
                                && event.isShiftPressed()
                                && event.isAltPressed()
                                && !event.isCtrlPressed())
                        || (event.getKeyCode() == KeyEvent.KEYCODE_ESCAPE && mReader.isShowing()))) {
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
                if (event.getKeyCode() == KeyEvent.KEYCODE_ESCAPE) {
                    mReader.leave();
                } else {
                    mReader.toggle();
                }
            }
            return true;
        }
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

    /** Compass's long-press menu in place of Chrome's (LinkMenu); null: Chrome's. */
    public org.chromium.components.embedder_support.contextmenu.@Nullable ContextMenuUi
            createContextMenu(
                    org.chromium.components.embedder_support.contextmenu.ContextMenuNativeDelegate
                            nativeDelegate) {
        return mLinkMenu != null ? mLinkMenu.prepare(nativeDelegate) : null;
    }

    /** "Peek" in a link's long-press menu in |source|: the link in Peek's card. */
    public boolean peekLink(Tab source, GURL url, @Nullable Origin initiator) {
        if (mPeek == null) return false;
        mPeek.showUrl(source, url, initiator);
        return true;
    }

    /**
     * A page |source| opens in a new tab: peeked if |source| is a pinned tab or favorite, so a
     * pinned app stays put (docs/design/peek.md, "Links from pinned tabs"); else false.
     */
    public boolean peekNewContents(Tab source, WebContents contents) {
        if (mPeek == null || source.isIncognito() || pinnedNodeFor(source.getId()) == null) {
            return false;
        }
        mPeek.showContents(source, contents);
        return true;
    }

    /**
     * A link from another app (docs/design/android.md, "Links routed to spaces"): a new Today tab
     * of the space its domain's rule names, shown; else the little window (Peek's card, of the
     * window's profile), or with that off a new Today tab of the shown space. Never incognito. At
     * a cold start it waits for the spaces, which hold the rules.
     */
    public void openLinkFromOtherApp(GURL url) {
        Spaces spaces = mSpaces;
        if (spaces == null || mPeek == null) return;
        spaces.runWhenLoaded(
                () -> {
                    if (mPeek == null) return;
                    spaces.onDismiss();
                    String ruleSpace = spaces.bridge().linkRuleSpace(url.getSpec());
                    if (!ruleSpace.isEmpty()) {
                        openInSpace(ruleSpace, url);
                    } else if (LittleWindow.isOn()) {
                        mPeek.showLittleWindow(
                                mActivity.getTabModelSelector().getModel(false).getProfile(),
                                url);
                    } else {
                        openInSpace(spaces.shownSpaceId(), url);
                    }
                });
    }

    // A new Today tab of |spaceId| (this window's profile), shown; the window switches to it.
    private void openInSpace(String spaceId, GURL url) {
        if (mSpaces != null && !spaceId.isEmpty() && !spaceId.equals(mSpaces.shownSpaceId())) {
            mSpaces.onSpaceClicked(spaceId);
        }
        mActivity
                .getTabCreator(false)
                .createNewTab(
                        new org.chromium.content_public.browser.LoadUrlParams(
                                url,
                                org.chromium.ui.base.PageTransition.LINK
                                        | org.chromium.ui.base.PageTransition.FROM_API),
                        org.chromium.chrome.browser.tab.TabLaunchType.FROM_EXTERNAL_APP,
                        null);
    }

    /** The window's spaces (Settings' Sites in Spaces). */
    @Nullable Spaces spaces() {
        return mSpaces;
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
    /** The page's site: Compass's Site sheet, else (a Compass or chrome:// page) Chrome's info. */
    void showSiteInfo() {
        Tab tab = mActivity.getActivityTab();
        if (tab != null && mSiteSheet != null && mSiteSheet.show(tab)) return;
        chromeAction("info_menu_id");
    }

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

    /** The chain's profiles and their spaces (Settings › Profiles). */
    java.util.List<CompassSettingsUi.ProfileRow> profileRows() {
        return mSpaces != null ? mSpaces.profileRows() : java.util.Collections.emptyList();
    }

    /** The window's space's whole theme. */
    SpaceTheme spaceTheme() {
        return mSpaceTheme;
    }

    java.util.List<ExtensionsState.Action> extensionActions() {
        return mExtensions != null ? mExtensions.actions() : java.util.Collections.emptyList();
    }

    void runExtension(String id) {
        if (mExtensions != null) mExtensions.onRunAction(id);
    }

    /** Capture to Easel: the visible page, to a board the user picks (Easels.java). */
    void captureToEasel() {
        Easels.capture(mActivity, mSpaces);
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
        String url = CopyLinkBridge.linkToCopy(tab.getProfile(), tab.getUrl().getSpec());
        clipboard.setPrimaryClip(ClipData.newPlainText("URL", url));
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
        if (mTabPicker != null) mTabPicker.show(mSpaceTheme);
    }

    /** Chrome's own tab switcher (tab groups). */
    public void showTabSwitcher() {
        mActivity.getLayoutManager().showLayout(LayoutType.HUB, /* animate= */ true);
    }

    /** Compass's Downloads screen (DownloadsScreen), in place of Chrome's download home. */
    public void showDownloads() {
        if (mDownloads != null) mDownloads.show();
    }

    /** Compass's History screen (HistoryScreen), in place of Chrome's. */
    public void showHistory() {
        if (mHistory != null) mHistory.show();
    }

    public void showClaude() {
        if (mClaude != null) mClaude.show();
    }

    /** Continue in Chat (Ask in the command bar): the Claude sheet, on that exchange. */
    public void continueClaude(String question, String sent, String answer) {
        if (mClaude != null) mClaude.continueWith(question, sent, answer);
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

    /** The tab shown is in Reader. */
    boolean inReader() {
        return mReader != null && mReader.isShowing();
    }

    @Override
    public void toggleReader() {
        if (mReader != null) mReader.toggle();
    }

    @Override
    public void findInPage() {
        // Compass's find bar: Chrome's is part of the toolbar the shell hides.
        if (mFindBar != null) mFindBar.show();
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

    // Follows |tab|'s page's scrolling, and shows the bar.
    private void watchScroll(@Nullable Tab tab) {
        WebContents contents = tab != null ? tab.getWebContents() : null;
        if (contents != mScrollContents) {
            if (mScrollContents != null && !mScrollContents.isDestroyed()) {
                GestureListenerManager.fromWebContents(mScrollContents)
                        .removeListener(mScrollListener);
            }
            mScrollContents = contents;
            if (contents != null) {
                GestureListenerManager.fromWebContents(contents)
                        .addListener(mScrollListener, RootScrollOffsetUpdateFrequency.ON_SCROLL_END);
            }
        }
        setBarHidden(false);
    }

    // The bar's margin over the system's navigation bar (gesture handle or buttons).
    private void keepClearOfNavBar() {
        if (mFrame == null) return;
        WindowInsetsCompat insets =
                ViewCompat.getRootWindowInsets(mActivity.getWindow().getDecorView());
        int bottom =
                insets == null
                        ? 0
                        : insets.getInsets(WindowInsetsCompat.Type.navigationBars()).bottom;
        ViewGroup.MarginLayoutParams params =
                (ViewGroup.MarginLayoutParams) mFrame.getLayoutParams();
        // The coordinator may end above the navigation bar already.
        View coordinator = (View) mFrame.getParent();
        int[] at = new int[2];
        coordinator.getLocationInWindow(at);
        int below = mActivity.getWindow().getDecorView().getHeight() - (at[1] + coordinator.getHeight());
        int margin = Math.max(0, bottom - Math.max(0, below));
        if (params.bottomMargin != margin) {
            params.bottomMargin = margin;
            mFrame.setLayoutParams(params);
        }
        pushSafeArea();
    }

    /**
     * How far above |view|'s bottom what the bar covers of its window ends (px, 0 if it covers
     * none of it): where a dialog or message docks.
     */
    static int aboveBar(View view) {
        Activity activity = org.chromium.base.ContextUtils.activityFromContext(view.getContext());
        if (activity == null) return 0;
        int covered = DisplayCutoutController.getCompassBottomInset(activity.getWindow());
        int[] at = new int[2];
        view.getLocationInWindow(at);
        int below = activity.getWindow().getDecorView().getHeight() - (at[1] + view.getHeight());
        return Math.max(0, covered - below);
    }

    // A viewport-fit=cover page's bottom safe area takes in the bar while it shows, so its fixed
    // bottom UI (bottom: env(safe-area-inset-bottom)) sits above it, as in Safari.
    private void pushSafeArea() {
        if (mFrame == null) return;
        int covered = 0;
        if (!mBarHidden) {
            int[] at = new int[2];
            mFrame.getLocationInWindow(at);
            // Where the bar's capsule ends at the top, without the slide's translation.
            int top = at[1] - Math.round(mFrame.getTranslationY());
            covered = Math.max(0, mActivity.getWindow().getDecorView().getHeight() - top);
        }
        DisplayCutoutController.setCompassBottomInset(mActivity.getWindow(), covered);
    }

    // Slides the bar away below the window's bottom, or back.
    private void setBarHidden(boolean hidden) {
        if (mFrame == null || hidden == mBarHidden) return;
        mBarHidden = hidden;
        pushSafeArea();
        mFrame.animate()
                .translationY(
                        hidden
                                ? mFrame.getHeight()
                                        + ((ViewGroup.MarginLayoutParams) mFrame.getLayoutParams())
                                                .bottomMargin
                                        + mHeightPx / 4f
                                : 0)
                .setDuration(200)
                .start();
    }
}
