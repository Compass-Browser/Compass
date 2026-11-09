// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.graphics.Bitmap;
import android.util.Size;
import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabCreationState;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.tab.TabSelectionType;
import org.chromium.chrome.browser.tab_ui.TabContentManager;
import org.chromium.chrome.browser.tabmodel.TabClosureParams;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.tabmodel.TabModelObserver;
import org.chromium.chrome.browser.tabmodel.TabModelSelector;
import org.chromium.chrome.browser.tabmodel.TabModelUtils;
import org.chromium.components.embedder_support.util.UrlConstants;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.ui.base.PageTransition;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/**
 * The tab picker (docs/design/android.md, "Tab picker"): the window's tabs as cards
 * (TabPickerUi.kt), in place of Chrome's tab switcher, which stays one step away on a long press.
 */
public class TabPicker implements TabPickerUi.Listener {
    /** A card's thumbnail, at most this wide (the cards are about 70% of a phone's width). */
    private static final int THUMBNAIL_WIDTH_PX = 720;

    private final ChromeTabbedActivity mActivity;
    private final TabPickerState mState = new TabPickerState();
    private final View mView;
    private final Runnable mOnNewTab;
    // The models observed while the picker is open (the activity's selector is gone by the time
    // it's destroyed).
    private final List<TabModel> mObserved = new ArrayList<>();
    private final TabModelObserver mObserver =
            new TabModelObserver() {
                @Override
                public void didAddTab(
                        Tab tab,
                        @TabLaunchType int type,
                        @TabCreationState int creationState,
                        boolean markedForSelection) {
                    refresh();
                }

                @Override
                public void didRemoveTabForClosure(Tab tab) {
                    refresh();
                }

                @Override
                public void tabRemoved(Tab tab) {
                    refresh();
                }

                @Override
                public void tabClosureUndone(Tab tab) {
                    refresh();
                }
            };

    /** |onNewTab| opens a regular new tab (the command bar, for one). */
    public TabPicker(ChromeTabbedActivity activity, ViewGroup parent, Runnable onNewTab) {
        mActivity = activity;
        mOnNewTab = onNewTab;
        mView = TabPickerUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    public void destroy() {
        stopObserving();
    }

    /** Opens the picker on the window's current tabs (regular or incognito), over |tint|. */
    public void show(String tint) {
        if (mState.getOpen()) return;
        mState.setTint(tint);
        View content = mActivity.getCompositorViewHolderSupplier().get();
        if (content != null && content.getWidth() > 0 && content.getHeight() > 0) {
            mState.setAspect((float) content.getWidth() / content.getHeight());
        }
        TabModelSelector selector = mActivity.getTabModelSelector();
        stopObserving();
        for (boolean incognito : new boolean[] {false, true}) {
            TabModel model = selector.getModel(incognito);
            model.addObserver(mObserver);
            mObserved.add(model);
        }
        mState.getThumbnails().clear();
        refresh();
        // The tab on screen, as it is now; the others' come from disk.
        Tab current = mActivity.getActivityTab();
        TabContentManager thumbnails = mActivity.getTabContentManager();
        if (current != null && thumbnails != null) {
            int id = current.getId();
            thumbnails.cacheTabThumbnailWithCallback(
                    current,
                    /* returnBitmap= */ true,
                    bitmap -> {
                        if (bitmap != null) mState.getThumbnails().put(id, bitmap);
                    });
        }
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
    }

    private void stopObserving() {
        for (TabModel model : mObserved) model.removeObserver(mObserver);
        mObserved.clear();
    }

    private void refresh() {
        TabModelSelector selector = mActivity.getTabModelSelector();
        TabModel model = selector.getCurrentModel();
        List<Tab> tabs = new ArrayList<>();
        for (int i = 0; i < model.getCount(); i++) tabs.add(model.getTabAt(i));
        // Least recently shown first, as on iOS: the tab on screen is the last.
        tabs.sort(Comparator.comparingLong(Tab::getTimestampMillis));
        List<TabPickerState.Card> cards = new ArrayList<>();
        TabContentManager thumbnails = mActivity.getTabContentManager();
        for (Tab tab : tabs) {
            String host = tab.getUrl().getHost();
            cards.add(new TabPickerState.Card(tab.getId(), tab.getTitle(), host));
            int id = tab.getId();
            if (thumbnails != null && !mState.getThumbnails().containsKey(id)) {
                thumbnails.getTabThumbnailWithCallback(
                        id,
                        thumbnailSize(),
                        (@Nullable Bitmap bitmap) -> {
                            // The live capture of the current tab wins over the disk's.
                            if (bitmap != null && !mState.getThumbnails().containsKey(id)) {
                                mState.getThumbnails().put(id, bitmap);
                            }
                        });
            }
        }
        Tab current = model.getTabAt(model.index());
        mState.setCurrentId(current != null ? current.getId() : Tab.INVALID_TAB_ID);
        mState.setIncognito(model.isIncognito());
        mState.setHasIncognito(model.isIncognito() || selector.getModel(true).getCount() > 0);
        mState.setCards(cards);
    }

    private Size thumbnailSize() {
        float aspect = mState.getAspect() > 0 ? mState.getAspect() : 0.5f;
        return new Size(THUMBNAIL_WIDTH_PX, Math.round(THUMBNAIL_WIDTH_PX / aspect));
    }

    private @Nullable Tab findTab(int id) {
        TabModelSelector selector = mActivity.getTabModelSelector();
        Tab tab = selector.getModel(false).getTabById(id);
        return tab != null ? tab : selector.getModel(true).getTabById(id);
    }

    // TabPickerUi.Listener:

    @Override
    public void onSelect(int id) {
        TabModelUtils.selectTabById(mActivity.getTabModelSelector(), id, TabSelectionType.FROM_USER);
        onDismiss();
    }

    @Override
    public void onClose(int id) {
        Tab tab = findTab(id);
        if (tab == null) return;
        TabModel model = mActivity.getTabModelSelector().getModel(tab.isIncognito());
        // Undo comes from Chrome's snackbar, as for any closed tab.
        model.getTabRemover()
                .closeTabs(
                        TabClosureParams.closeTab(tab).allowUndo(true).build(),
                        /* allowDialog= */ false);
        mState.getThumbnails().remove(id);
        refresh();
    }

    @Override
    public void onNewTab() {
        boolean incognito = mState.getIncognito();
        onDismiss();
        if (incognito) {
            newIncognitoTab();
        } else {
            mOnNewTab.run();
        }
    }

    @Override
    public void onIncognito(boolean incognito) {
        TabModelSelector selector = mActivity.getTabModelSelector();
        if (incognito && selector.getModel(true).getCount() == 0) {
            // Nothing to show there yet: a new incognito tab.
            onDismiss();
            newIncognitoTab();
            return;
        }
        selector.selectModel(incognito);
        refresh();
    }

    private void newIncognitoTab() {
        mActivity
                .getTabCreator(true)
                .createNewTab(
                        new LoadUrlParams(UrlConstants.NTP_URL, PageTransition.AUTO_TOPLEVEL),
                        TabLaunchType.FROM_CHROME_UI,
                        null);
    }

    @Override
    public void onDismiss() {
        stopObserving();
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
        mState.getThumbnails().clear();
    }
}
