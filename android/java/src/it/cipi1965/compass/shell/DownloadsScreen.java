// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.download.items.OfflineContentAggregatorFactory;
import org.chromium.chrome.browser.settings.SettingsNavigationFactory;
import org.chromium.components.browser_ui.settings.SettingsNavigation.SettingsFragment;
import org.chromium.components.offline_items_collection.ContentId;
import org.chromium.components.offline_items_collection.LaunchLocation;
import org.chromium.components.offline_items_collection.OfflineContentProvider;
import org.chromium.components.offline_items_collection.OfflineItem;
import org.chromium.components.offline_items_collection.OfflineItemState;
import org.chromium.components.offline_items_collection.OpenParams;
import org.chromium.components.offline_items_collection.UpdateDelta;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Compass's Downloads screen (DownloadsUi.kt), from the page menu, in place of Chrome's download
 * home: the profile's downloads from Chrome's OfflineContentProvider, kept up to date while open.
 */
public class DownloadsScreen implements DownloadsUi.Listener, OfflineContentProvider.Observer {
    private final ChromeTabbedActivity mActivity;
    private final DownloadsUi.State mState = new DownloadsUi.State();
    private final View mView;
    // The shown downloads by key (namespace and ID).
    private final Map<String, OfflineItem> mItems = new LinkedHashMap<>();
    private boolean mObserving;

    public DownloadsScreen(ChromeTabbedActivity activity, ViewGroup parent) {
        mActivity = activity;
        mView = DownloadsUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private static OfflineContentProvider provider() {
        return OfflineContentAggregatorFactory.get();
    }

    public void show() {
        if (!mObserving) {
            provider().addObserver(this);
            mObserving = true;
        }
        provider()
                .getAllItems(
                        items -> {
                            mItems.clear();
                            for (OfflineItem item : items) add(item);
                            publish();
                        });
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
    }

    public void destroy() {
        if (mObserving) provider().removeObserver(this);
        mObserving = false;
    }

    private static String key(ContentId id) {
        return id.namespace + "," + id.id;
    }

    // Downloads only: not Incognito's, nor the transient or the ones whose file is gone.
    private void add(OfflineItem item) {
        if (item.id == null
                || item.isTransient
                || item.isOffTheRecord
                || item.externallyRemoved
                || item.state == OfflineItemState.CANCELLED) {
            if (item.id != null) mItems.remove(key(item.id));
            return;
        }
        mItems.put(key(item.id), item);
    }

    private void publish() {
        List<OfflineItem> items = new ArrayList<>(mItems.values());
        items.sort((a, b) -> Long.compare(b.creationTimeMs, a.creationTimeMs));
        List<DownloadsUi.Download> downloads = new ArrayList<>();
        for (OfflineItem item : items) {
            DownloadsUi.Status status;
            switch (item.state) {
                case OfflineItemState.COMPLETE -> status = DownloadsUi.Status.DONE;
                case OfflineItemState.PAUSED, OfflineItemState.INTERRUPTED ->
                        status = DownloadsUi.Status.PAUSED;
                case OfflineItemState.FAILED -> status = DownloadsUi.Status.FAILED;
                default -> status = DownloadsUi.Status.RUNNING;
            }
            int percent =
                    item.progress != null && !item.progress.isIndeterminate()
                            ? item.progress.getPercentage()
                            : -1;
            String host =
                    item.originalUrl != null && item.originalUrl.isValid()
                            ? item.originalUrl.getHost()
                            : item.url != null && item.url.isValid() ? item.url.getHost() : "";
            downloads.add(
                    new DownloadsUi.Download(
                            key(item.id),
                            item.title,
                            host.startsWith("www.") ? host.substring(4) : host,
                            status,
                            item.receivedBytes,
                            Math.max(0, item.totalSizeBytes),
                            percent,
                            item.completionTimeMs > 0 ? item.completionTimeMs : item.creationTimeMs));
        }
        mState.setDownloads(downloads);
    }

    // OfflineContentProvider.Observer:

    @Override
    public void onItemsAdded(List<OfflineItem> items) {
        for (OfflineItem item : items) add(item);
        publish();
    }

    @Override
    public void onItemRemoved(ContentId id) {
        mItems.remove(key(id));
        publish();
    }

    @Override
    public void onItemUpdated(OfflineItem item, UpdateDelta updateDelta) {
        add(item);
        publish();
    }

    // DownloadsUi.Listener:

    private ContentId idOf(String key) {
        OfflineItem item = mItems.get(key);
        return item != null && item.id != null ? item.id : new ContentId();
    }

    @Override
    public void onOpen(String key) {
        OfflineItem item = mItems.get(key);
        if (item == null || item.id == null) return;
        provider().openItem(new OpenParams(LaunchLocation.DOWNLOAD_HOME), item.id);
    }

    @Override
    public void onPauseOrResume(String key) {
        OfflineItem item = mItems.get(key);
        if (item == null || item.id == null) return;
        if (item.state == OfflineItemState.PAUSED || item.state == OfflineItemState.INTERRUPTED) {
            provider().resumeDownload(item.id);
        } else {
            provider().pauseDownload(item.id);
        }
    }

    @Override
    public void onCancel(String key) {
        if (!mItems.containsKey(key)) return;
        provider().cancelDownload(idOf(key));
    }

    @Override
    public void onDelete(String key) {
        if (!mItems.containsKey(key)) return;
        provider().removeItem(idOf(key));
    }

    @Override
    public void onSettings() {
        SettingsNavigationFactory.createSettingsNavigation()
                .startSettings(mActivity, SettingsFragment.DOWNLOADS);
    }

    @Override
    public void onDismiss() {
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
        if (mObserving) provider().removeObserver(this);
        mObserving = false;
    }
}
