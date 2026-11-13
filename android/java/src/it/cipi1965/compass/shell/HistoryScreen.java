// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.history.BrowsingHistoryBridge;
import org.chromium.chrome.browser.history.HistoryItem;
import org.chromium.chrome.browser.history.HistoryProvider;
import org.chromium.chrome.browser.history.QueryOptions;
import org.chromium.chrome.browser.settings.SettingsNavigationFactory;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.components.browser_ui.settings.SettingsNavigation.SettingsFragment;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.ui.base.PageTransition;

import java.util.ArrayList;
import java.util.List;

/**
 * Compass's History screen (HistoryUi.kt), from the page menu, in place of Chrome's: the profile's
 * visits through Chrome's BrowsingHistoryBridge, a page at a time.
 */
public class HistoryScreen implements HistoryUi.Listener, HistoryProvider.BrowsingHistoryObserver {
    private final ChromeTabbedActivity mActivity;
    private final HistoryUi.State mState = new HistoryUi.State();
    private final View mView;
    private @Nullable BrowsingHistoryBridge mBridge;
    // Chrome's items, in the state's order (a Visit's index).
    private final List<HistoryItem> mItems = new ArrayList<>();
    // One query at a time (BrowsingHistoryBridge): what was typed while one ran comes next, and
    // the running one's results are dropped.
    private boolean mQuerying;
    private @Nullable String mNextQuery;

    public HistoryScreen(ChromeTabbedActivity activity, ViewGroup parent) {
        mActivity = activity;
        mView = HistoryUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    public void show() {
        if (mBridge == null) {
            mBridge =
                    new BrowsingHistoryBridge(
                            mActivity.getTabModelSelector().getModel(false).getProfile());
            mBridge.setObserver(this);
        }
        mState.setQuery("");
        query("");
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
    }

    public void destroy() {
        if (mBridge != null) mBridge.destroy();
        mBridge = null;
    }

    private void query(String query) {
        if (mBridge == null) return;
        if (mQuerying) {
            mNextQuery = query;
            return;
        }
        mQuerying = true;
        mItems.clear();
        mState.setVisits(new ArrayList<>());
        mState.setHasMore(false);
        mState.setLoading(true);
        mBridge.queryHistory(query, new QueryOptions());
    }

    // HistoryProvider.BrowsingHistoryObserver:

    @Override
    public void onQueryHistoryComplete(List<HistoryItem> items, boolean hasMorePotentialMatches) {
        mQuerying = false;
        if (mNextQuery != null) {
            String next = mNextQuery;
            mNextQuery = null;
            query(next);
            return;
        }
        List<HistoryUi.Visit> visits = new ArrayList<>(mState.getVisits());
        for (HistoryItem item : items) {
            if (item.wasBlockedVisit()) continue;
            mItems.add(item);
            visits.add(
                    new HistoryUi.Visit(
                            mItems.size() - 1,
                            item.getTitle(),
                            item.getUrl().getSpec(),
                            item.getDomain(),
                            item.getTimestamp()));
        }
        mState.setVisits(visits);
        mState.setHasMore(hasMorePotentialMatches);
        mState.setLoading(false);
    }

    @Override
    public void onHistoryDeleted() {
        if (mState.getOpen()) query(mState.getQuery());
    }

    @Override
    public void hasOtherFormsOfBrowsingData(boolean hasOtherForms) {}

    @Override
    public void onQueryAppsComplete(List<String> items) {}

    // HistoryUi.Listener:

    @Override
    public void onQuery(String query) {
        mState.setQuery(query);
        query(query.trim());
    }

    @Override
    public void onOpen(HistoryUi.Visit visit) {
        onDismiss();
        Tab tab = mActivity.getActivityTabProvider().get();
        LoadUrlParams params = new LoadUrlParams(visit.url, PageTransition.AUTO_BOOKMARK);
        if (tab != null && !tab.isIncognito()) {
            tab.loadUrl(params);
        } else {
            mActivity
                    .getTabCreator(false)
                    .createNewTab(params, TabLaunchType.FROM_CHROME_UI, null);
        }
    }

    @Override
    public void onRemove(HistoryUi.Visit visit) {
        if (mBridge == null || visit.index < 0 || visit.index >= mItems.size()) return;
        mBridge.markItemForRemoval(mItems.get(visit.index));
        mBridge.removeItems();
        List<HistoryUi.Visit> visits = new ArrayList<>();
        for (HistoryUi.Visit other : mState.getVisits()) {
            if (other != visit) visits.add(other);
        }
        mState.setVisits(visits);
    }

    @Override
    public void onMore() {
        if (mBridge == null || mState.getLoading()) return;
        mState.setLoading(true);
        mQuerying = true;
        mBridge.queryHistoryContinuation();
    }

    @Override
    public void onClearData() {
        SettingsNavigationFactory.createSettingsNavigation()
                .startSettings(mActivity, SettingsFragment.CLEAR_BROWSING_DATA);
    }

    @Override
    public void onDismiss() {
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
