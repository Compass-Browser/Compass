// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.graphics.Color;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import androidx.activity.OnBackPressedCallback;
import androidx.coordinatorlayout.widget.CoordinatorLayout;

import org.chromium.base.ThreadUtils;
import org.chromium.base.version_info.VersionInfo;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.content.WebContentsFactory;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.components.embedder_support.delegate.WebContentsDelegateAndroid;
import org.chromium.components.embedder_support.util.UrlUtilities;
import org.chromium.components.embedder_support.view.ContentView;
import org.chromium.components.thinwebview.ThinWebView;
import org.chromium.components.thinwebview.ThinWebViewAttachParams;
import org.chromium.components.thinwebview.ThinWebViewConstraints;
import org.chromium.components.thinwebview.ThinWebViewFactory;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.content_public.browser.NavigationHandle;
import org.chromium.content_public.browser.WebContents;
import org.chromium.content_public.browser.WebContentsObserver;
import org.chromium.content_public.common.ResourceRequestBody;
import org.chromium.ui.base.PageTransition;
import org.chromium.ui.base.ViewAndroidDelegate;
import org.chromium.url.GURL;

import java.util.HashMap;
import java.util.Map;

/**
 * Reader mode (docs/design/reader-mode.md; android.md, "Reader mode"): a page that reads as an
 * article gets a book in the bar's address; it shows the article over the tab, in the desktop's
 * reader page (chrome-untrusted://compass-reader, the shared common/reader/ files), and again goes
 * back to the page, untouched underneath. Reader is per tab: switching away takes the view away,
 * and coming back shows it again. A new document in the tab leaves it.
 *
 * <p>The reader page is a WebContents of the tab's profile in a ThinWebView over the page (under
 * the bar), as Peek's card; ReaderBridge runs the desktop's scripts in the page's isolated world.
 */
public class Reader {
    /** The bar's book: whether the tab shown reads as an article, and whether it's in Reader. */
    public interface Listener {
        void onReaderChanged(boolean readerable, boolean showing);
    }

    // After a same-document navigation (single-page apps), and once after load for pages that
    // build their text late, as on the desktop.
    private static final long RECHECK_MS = 1000;
    private static final long RECHECK_AFTER_LOAD_MS = 2000;

    private final ChromeTabbedActivity mActivity;
    private final Listener mListener;
    // Over the page, under the bar.
    private final FrameLayout mHost;
    private final Map<Integer, TabReader> mTabs = new HashMap<>();
    private @Nullable Tab mShown;
    private int mGeneration;

    private final OnBackPressedCallback mBack =
            new OnBackPressedCallback(false) {
                @Override
                public void handleOnBackPressed() {
                    leave();
                }
            };

    /** One tab's Reader: its article check, and its reader page while it's in Reader. */
    private class TabReader {
        final int mTabId;
        // The URL the check was for, and its answer.
        String mCheckedUrl = "";
        boolean mReaderable;
        boolean mParsing;
        // While in Reader: the article's page, what shows it, and the article's token.
        @Nullable WebContents mContents;
        @Nullable ThinWebView mView;
        @Nullable String mToken;
        // The page the article was read from: another document leaves Reader.
        String mArticleUrl = "";

        TabReader(int tabId) {
            mTabId = tabId;
        }

        boolean showing() {
            return mContents != null;
        }

        void close() {
            mParsing = false;
            ThinWebView view = mView;
            WebContents contents = mContents;
            String token = mToken;
            mView = null;
            mContents = null;
            mToken = null;
            if (view != null) {
                mHost.removeView(view.getView());
                view.destroy();
            }
            // After the current event: the page may be handling it (window.close()).
            if (contents != null) ThreadUtils.postOnUiThread(contents::destroy);
            if (token != null) ReaderBridge.release(token);
        }
    }

    public Reader(
            ChromeTabbedActivity activity, ViewGroup coordinator, View bar, Listener listener) {
        mActivity = activity;
        mListener = listener;
        mHost = new FrameLayout(activity);
        mHost.setVisibility(View.GONE);
        // Under the bar, which floats over the page and over the article alike.
        coordinator.addView(
                mHost,
                Math.max(0, coordinator.indexOfChild(bar)),
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        activity.getOnBackPressedDispatcher().addCallback(activity, mBack);
    }

    private TabReader readerOf(Tab tab) {
        TabReader reader = mTabs.get(tab.getId());
        if (reader == null) {
            reader = new TabReader(tab.getId());
            mTabs.put(tab.getId(), reader);
        }
        return reader;
    }

    public boolean isShowing() {
        TabReader reader = mShown != null ? mTabs.get(mShown.getId()) : null;
        return reader != null && reader.showing();
    }

    private static boolean canRead(GURL url) {
        return url.isValid() && (UrlUtilities.isHttpOrHttps(url) || "file".equals(url.getScheme()));
    }

    /** The window shows |tab|: its reader view if it's in Reader, and its book. */
    public void onTabShown(@Nullable Tab tab) {
        if (mShown == tab) return;
        mShown = tab;
        mHost.removeAllViews();
        mHost.setVisibility(View.GONE);
        if (tab != null) {
            TabReader reader = readerOf(tab);
            if (reader.showing() && !reader.mArticleUrl.equals(tab.getUrl().getSpec())) {
                // Another document came while the tab was away.
                reader.close();
            }
            if (reader.showing() && reader.mView != null) attachView(reader.mView);
            if (!tab.getUrl().getSpec().equals(reader.mCheckedUrl)) check(tab, 0);
        }
        update();
    }

    /** The shown tab finished loading a page. */
    public void onPageLoaded(Tab tab) {
        if (tab != mShown) return;
        check(tab, 0);
        // Pages that build their text late.
        check(tab, RECHECK_AFTER_LOAD_MS);
    }

    /** The shown tab navigated: a new document leaves Reader; a same-document one rechecks. */
    public void onNavigated(Tab tab, NavigationHandle navigation) {
        if (tab != mShown || !navigation.hasCommitted()) return;
        TabReader reader = readerOf(tab);
        if (navigation.isSameDocument()) {
            check(tab, RECHECK_MS);
            return;
        }
        reader.mCheckedUrl = "";
        reader.mReaderable = false;
        if (reader.showing() || reader.mParsing) {
            reader.close();
            mHost.removeAllViews();
            mHost.setVisibility(View.GONE);
        }
        update();
    }

    /** A tab went: its reader page goes too. */
    public void onTabClosed(int tabId) {
        TabReader reader = mTabs.remove(tabId);
        if (reader == null) return;
        boolean shown = mShown != null && mShown.getId() == tabId;
        reader.close();
        if (shown) {
            mHost.removeAllViews();
            mHost.setVisibility(View.GONE);
            mShown = null;
            update();
        }
    }

    private void check(Tab tab, long delayMs) {
        int generation = ++mGeneration;
        Runnable run =
                () -> {
                    if (tab != mShown || generation != mGeneration) return;
                    WebContents contents = tab.getWebContents();
                    TabReader reader = readerOf(tab);
                    String url = tab.getUrl().getSpec();
                    if (contents == null || !canRead(tab.getUrl())) {
                        reader.mCheckedUrl = url;
                        reader.mReaderable = false;
                        update();
                        return;
                    }
                    ReaderBridge.check(
                            contents,
                            readerable -> {
                                if (tab != mShown || !url.equals(tab.getUrl().getSpec())) return;
                                reader.mCheckedUrl = url;
                                reader.mReaderable = readerable;
                                update();
                            });
                };
        if (delayMs > 0) {
            ThreadUtils.postOnUiThreadDelayed(run, delayMs);
        } else {
            run.run();
        }
    }

    /** The book, the page menu's Reader, Shift+Alt+R: shows Reader, or goes back to the page. */
    public void toggle() {
        if (isShowing()) {
            leave();
        } else {
            enter();
        }
    }

    /**
     * Shows the shown tab's article. On a page that isn't an article it still shows what
     * Readability found, or the reader page says there's none, as on the desktop.
     */
    public void enter() {
        Tab tab = mShown;
        if (tab == null || tab.getWebContents() == null || !canRead(tab.getUrl())) return;
        TabReader reader = readerOf(tab);
        if (reader.showing() || reader.mParsing) return;
        reader.mParsing = true;
        String url = tab.getUrl().getSpec();
        ReaderBridge.parse(
                tab.getWebContents(),
                token -> {
                    if (!reader.mParsing || token.isEmpty()) {
                        if (!token.isEmpty()) ReaderBridge.release(token);
                        reader.mParsing = false;
                        return;
                    }
                    reader.mParsing = false;
                    if (tab.isDestroyed() || !url.equals(tab.getUrl().getSpec())) {
                        ReaderBridge.release(token);
                        return;
                    }
                    show(tab, reader, token);
                });
    }

    private void show(Tab tab, TabReader reader, String token) {
        WebContents contents =
                WebContentsFactory.createWebContents(
                        tab.getProfile(), /* initiallyHidden= */ false, /* initializeRenderer= */ false);
        ContentView contentView = ContentView.createContentView(mActivity, contents);
        contents.setDelegates(
                VersionInfo.getProductVersion(),
                ViewAndroidDelegate.createBasicDelegate(contentView),
                contentView,
                mActivity.getWindowAndroid(),
                WebContents.createDefaultInternalsHolder());
        ThinWebViewConstraints constraints = new ThinWebViewConstraints();
        constraints.supportsOpacity = true;
        constraints.backgroundColor = Color.WHITE;
        ThinWebView view =
                ThinWebViewFactory.create(
                        mActivity,
                        constraints,
                        mActivity.getWindowAndroid().getIntentRequestTracker(),
                        /* enablePermissionRequests= */ false);
        view.attachWebContents(
                contents,
                contentView,
                new ThinWebViewAttachParams.Builder()
                        .setWebContentsDelegate(new Delegate(tab))
                        .build());
        new LinkObserver(contents, tab);
        reader.mContents = contents;
        reader.mView = view;
        reader.mToken = token;
        reader.mArticleUrl = tab.getUrl().getSpec();
        contents.getNavigationController()
                .loadUrl(
                        new LoadUrlParams(
                                ReaderBridge.pageUrl(token), PageTransition.AUTO_TOPLEVEL));
        if (tab == mShown) attachView(view);
        update();
    }

    private void attachView(ThinWebView view) {
        mHost.removeAllViews();
        mHost.addView(
                view.getView(),
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        mHost.setVisibility(View.VISIBLE);
        view.getView().requestFocus();
    }

    /** Back to the page: the reader page goes. */
    public void leave() {
        Tab tab = mShown;
        if (tab == null) return;
        TabReader reader = mTabs.get(tab.getId());
        if (reader == null || (!reader.showing() && !reader.mParsing)) return;
        reader.close();
        mHost.removeAllViews();
        mHost.setVisibility(View.GONE);
        update();
    }

    private void update() {
        TabReader reader = mShown != null ? mTabs.get(mShown.getId()) : null;
        boolean showing = reader != null && reader.showing();
        mBack.setEnabled(showing);
        mListener.onReaderChanged(reader != null && reader.mReaderable, showing);
    }

    public void destroy() {
        for (TabReader reader : mTabs.values()) reader.close();
        mTabs.clear();
        mBack.remove();
    }

    // The reader page's delegate: × (window.close()) leaves; links that open a tab open one, as
    // from the page.
    private class Delegate extends WebContentsDelegateAndroid {
        private final Tab mTab;

        Delegate(Tab tab) {
            mTab = tab;
        }

        @Override
        public void closeContents() {
            if (mTab == mShown) leave();
        }

        @Override
        public void openNewTab(
                GURL url,
                String extraHeaders,
                @Nullable ResourceRequestBody postData,
                int disposition,
                boolean isRendererInitiated) {
            openTab(url);
        }

        @Override
        public boolean shouldCreateWebContents(GURL targetUrl) {
            openTab(targetUrl);
            return false;
        }

        private void openTab(GURL url) {
            if (!UrlUtilities.isHttpOrHttps(url)) return;
            mActivity
                    .getTabCreator(mTab.isIncognito())
                    .createNewTab(new LoadUrlParams(url, PageTransition.LINK), TabLaunchType.FROM_LINK, mTab);
        }
    }

    // A plain link in the article loads in the tab, which leaves Reader; the reader page itself
    // never leaves chrome-untrusted://compass-reader.
    private class LinkObserver extends WebContentsObserver {
        private final Tab mTab;

        LinkObserver(WebContents contents, Tab tab) {
            super(contents);
            mTab = tab;
        }

        @Override
        public void didStartNavigationInPrimaryMainFrame(NavigationHandle navigation) {
            GURL url = navigation.getUrl();
            if ("chrome-untrusted".equals(url.getScheme())) return;
            WebContents contents = getWebContents();
            if (contents != null) contents.stop();
            if (mTab.isDestroyed()) return;
            if (UrlUtilities.isHttpOrHttps(url)) {
                // The tab's new document leaves Reader (onNavigated()).
                mTab.loadUrl(new LoadUrlParams(url, PageTransition.LINK));
            }
        }
    }
}
