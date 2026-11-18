// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.graphics.Color;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import org.chromium.base.ThreadUtils;
import org.chromium.base.version_info.VersionInfo;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.content.WebContentsFactory;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.search_engines.TemplateUrlServiceFactory;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.tabmodel.TabCreator;
import org.chromium.components.embedder_support.delegate.WebContentsDelegateAndroid;
import org.chromium.components.embedder_support.util.UrlUtilities;
import org.chromium.components.embedder_support.view.ContentView;
import org.chromium.components.thinwebview.ThinWebView;
import org.chromium.components.thinwebview.ThinWebViewAttachParams;
import org.chromium.components.thinwebview.ThinWebViewConstraints;
import org.chromium.components.thinwebview.ThinWebViewFactory;
import org.chromium.components.url_formatter.UrlFormatter;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.content_public.browser.WebContents;
import org.chromium.content_public.common.ResourceRequestBody;
import org.chromium.ui.base.IntentRequestTracker;
import org.chromium.ui.base.PageTransition;
import org.chromium.ui.base.ViewAndroidDelegate;
import org.chromium.url.GURL;
import org.chromium.url.Origin;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;

/**
 * Peek (docs/design/peek.md; android.md, "Peek"): a link's page in a card over the window, without
 * a tab. Closing the card lets the page go; Open makes that same page (its WebContents, so no
 * reload) a new tab of the window, which the spaces make a Today entry. In its little-window mode
 * (android.md, "Little windows") the card holds a link from another app, with back, the address
 * and Open in <space>.
 *
 * <p>The page is a WebContents of the source's profile in a ThinWebView, as Chrome's "Preview
 * page" (EphemeralTabCoordinator), which has its own compositor; ThinWebView attaches the tab
 * helpers a tab has (TabHelpers::AttachTabHelpers), so the page keeps them when a tab adopts it
 * (ChromeTabCreator.createTabWithWebContents()).
 */
public class Peek implements PeekUi.Listener {
    private final ChromeTabbedActivity mActivity;
    private final PeekUi.State mState = new PeekUi.State();
    private final View mView;
    // The page's place in the card; the ThinWebView's view goes in it.
    private final FrameLayout mPageHost;
    private final Spaces mSpaces;

    private @Nullable WebContents mContents;
    private @Nullable ContentView mContentView;
    private @Nullable ThinWebView mThinWebView;
    private @Nullable WebContentsDelegateAndroid mDelegate;
    private boolean mIncognito;
    // The tab the card is over: another one shown closes it.
    private int mOverTab = Tab.INVALID_TAB_ID;

    public Peek(ChromeTabbedActivity activity, ViewGroup parent, Spaces spaces) {
        mActivity = activity;
        mSpaces = spaces;
        mPageHost = new FrameLayout(activity);
        mView =
                PeekUi.createView(
                        activity, activity, activity, activity, mState, mPageHost, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    public boolean isShowing() {
        return mContents != null;
    }

    /** Peeks |url|, from a link's long-press menu in |source|. */
    public void showUrl(Tab source, GURL url, @Nullable Origin initiator) {
        if (!UrlUtilities.isHttpOrHttps(url)) return;
        WebContents contents = createContents(source.getProfile());
        LoadUrlParams params = new LoadUrlParams(url, PageTransition.LINK);
        if (initiator != null) params.setInitiatorOrigin(initiator);
        show(contents, setDelegates(contents), source.isIncognito(), /* little= */ false);
        contents.getNavigationController().loadUrl(params);
    }

    /**
     * Peeks a page a pinned tab or favorite opened (target=_blank, window.open()): the
     * WebContents Chrome would have made a tab of, so it keeps its opener.
     */
    public void showContents(Tab source, WebContents contents) {
        show(contents, setDelegates(contents), source.isIncognito(), /* little= */ false);
    }

    /** A link from another app, in the little window's card. */
    public void showLittleWindow(Profile profile, GURL url) {
        WebContents contents = createContents(profile);
        show(contents, setDelegates(contents), /* incognito= */ false, /* little= */ true);
        contents.getNavigationController()
                .loadUrl(new LoadUrlParams(url, PageTransition.LINK | PageTransition.FROM_API));
    }

    private static WebContents createContents(Profile profile) {
        return WebContentsFactory.createWebContents(
                profile, /* initiallyHidden= */ true, /* initializeRenderer= */ false);
    }

    private ContentView setDelegates(WebContents contents) {
        ContentView contentView = ContentView.createContentView(mActivity, contents);
        contents.setDelegates(
                VersionInfo.getProductVersion(),
                ViewAndroidDelegate.createBasicDelegate(contentView),
                contentView,
                mActivity.getWindowAndroid(),
                WebContents.createDefaultInternalsHolder());
        return contentView;
    }

    private void show(
            WebContents contents, ContentView contentView, boolean incognito, boolean little) {
        // A second link replaces the first.
        if (mContents != null) letGo(/* destroy= */ true);
        mContents = contents;
        mContentView = contentView;
        mIncognito = incognito;
        Tab over = mActivity.getActivityTab();
        mOverTab = over != null ? over.getId() : Tab.INVALID_TAB_ID;

        ThinWebViewConstraints constraints = new ThinWebViewConstraints();
        // A TextureView: the card clips it to its corners and animates it.
        constraints.supportsOpacity = true;
        constraints.backgroundColor = Color.WHITE;
        IntentRequestTracker tracker = mActivity.getWindowAndroid().getIntentRequestTracker();
        ThinWebView thinWebView =
                ThinWebViewFactory.create(
                        mActivity, constraints, tracker, /* enablePermissionRequests= */ true);
        mThinWebView = thinWebView;
        mDelegate = new Delegate();
        thinWebView.attachWebContents(
                contents,
                contentView,
                new ThinWebViewAttachParams.Builder()
                        .setWebContentsDelegate(mDelegate)
                        .setSupportTheming(true)
                        .build());
        PeekBridge.prepareForTab(contents);
        mPageHost.removeAllViews();
        mPageHost.addView(
                thinWebView.getView(),
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        mState.setLittle(little);
        mState.setAddress("");
        mState.setEditing(false);
        updateSpaces();
        update();
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
    }

    private void updateSpaces() {
        SidebarState sidebar = mSpaces.state();
        SidebarState.Space shown = sidebar.getActiveSpace();
        mState.setSpaceId(shown != null ? shown.id : "");
        mState.setSpaceName(shown != null ? shown.name : "");
        List<PeekUi.SpaceRow> rows = new ArrayList<>();
        for (SidebarState.Space space : sidebar.getSpaces()) {
            rows.add(new PeekUi.SpaceRow(space.id, space.name, space.emoji, space.tint));
        }
        mState.setSpaces(rows);
        mState.setTheme(shown != null && !mIncognito ? shown.theme : null);
    }

    private void update() {
        WebContents contents = mContents;
        if (contents == null) return;
        GURL url = contents.getVisibleUrl();
        mState.setTitle(contents.getTitle());
        mState.setHost(url.getHost());
        mState.setUrl(url.getSpec());
        mState.setCanGoBack(contents.getNavigationController().canGoBack());
        mState.setLoading(contents.isLoading());
        // The little window's Always Open <domain> in (link-routing.md): the page's domain as a
        // rule's, and the space its rule names.
        String domain =
                mState.getLittle() && UrlUtilities.isHttpOrHttps(url)
                        ? mSpaces.bridge().normalizeLinkDomain(url.getSpec())
                        : "";
        mState.setRuleDomain(domain);
        mState.setRuleSpaceId(domain.isEmpty() ? "" : mSpaces.bridge().linkRuleFor(domain));
    }

    /** Always Open <domain> in <space>: makes (or changes) the rule, and opens the page there. */
    @Override
    public void onAlwaysOpenIn(String spaceId) {
        String domain = mState.getRuleDomain();
        if (!domain.isEmpty()) mSpaces.bridge().setLinkRule(domain, spaceId);
        onOpen(spaceId);
    }

    /**
     * The window shows |tab|: another tab than the one the card is over closes it, as on iOS. A
     * card that came before any tab (a cold start with a link from another app) stays over the
     * first one.
     */
    public void onTabShown(@Nullable Tab tab) {
        if (!isShowing() || tab == null) return;
        if (mOverTab == Tab.INVALID_TAB_ID) {
            mOverTab = tab.getId();
        } else if (tab.getId() != mOverTab) {
            onClose();
        }
    }

    /** Closes the card; the page goes. */
    @Override
    public void onClose() {
        if (mContents == null) return;
        letGo(/* destroy= */ true);
        hide();
    }

    /**
     * Open (or Open in <space>): the page becomes the active tab of the window, a new Today entry
     * of |spaceId| (empty: the space shown), as it is.
     */
    @Override
    public void onOpen(String spaceId) {
        WebContents contents = mContents;
        if (contents == null) return;
        if (!spaceId.isEmpty() && !spaceId.equals(mSpaces.shownSpaceId())) {
            // Same profile only: a page can't change its website data.
            mSpaces.onSpaceClicked(spaceId);
        }
        boolean incognito = mIncognito;
        // Off the card first: the new tab's selection would close it, and the page is the tab's.
        mContents = null;
        TabCreator creator = mActivity.getTabCreator(incognito);
        Tab tab =
                creator.createTabWithWebContents(
                        /* parent= */ null,
                        /* shouldPin= */ false,
                        contents,
                        TabLaunchType.FROM_LINK,
                        CompletableFuture.completedFuture(true));
        // The tab took the page (and moved it to the window's compositor); the card's view goes.
        letGo(/* destroy= */ tab == null);
        if (tab == null) contents.destroy();
        hide();
    }

    @Override
    public void onBack() {
        if (mContents != null && mContents.getNavigationController().canGoBack()) {
            mContents.getNavigationController().goBack();
        }
    }

    @Override
    public void onReload() {
        if (mContents != null) mContents.getNavigationController().reload(true);
    }

    @Override
    public void onEditAddress(boolean editing) {
        mState.setAddress(editing ? mState.getUrl() : "");
        mState.setEditing(editing);
    }

    @Override
    public void onAddressChanged(String text) {
        mState.setAddress(text);
    }

    /** Go in the address: an address, or else a search, in the card. */
    @Override
    public void onGo(String text) {
        WebContents contents = mContents;
        if (contents == null) return;
        String typed = text.trim();
        mState.setEditing(false);
        if (typed.isEmpty()) return;
        GURL url = UrlFormatter.fixupUrl(typed);
        if (!typed.contains(" ") && url.isValid() && UrlUtilities.isHttpOrHttps(url)
                && (typed.contains(".") || typed.contains(":"))) {
            contents.getNavigationController()
                    .loadUrl(new LoadUrlParams(url, PageTransition.TYPED));
            return;
        }
        Profile profile = Profile.fromWebContents(contents);
        if (profile == null) return;
        String search =
                TemplateUrlServiceFactory.getForProfile(profile).getUrlForSearchQuery(typed);
        contents.getNavigationController()
                .loadUrl(new LoadUrlParams(search, PageTransition.GENERATED));
    }

    /** The card's keys, before the page and Chrome's: Esc, Ctrl+W, Ctrl+O, Ctrl+L, Ctrl+R… */
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (!isShowing()) return false;
        int key = event.getKeyCode();
        boolean ctrl = event.isCtrlPressed() && !event.isAltPressed() && !event.isMetaPressed();
        boolean ours =
                key == KeyEvent.KEYCODE_ESCAPE
                        || (ctrl
                                && !event.isShiftPressed()
                                && (key == KeyEvent.KEYCODE_W
                                        || key == KeyEvent.KEYCODE_O
                                        || key == KeyEvent.KEYCODE_R
                                        || key == KeyEvent.KEYCODE_LEFT_BRACKET
                                        || (key == KeyEvent.KEYCODE_L && mState.getLittle())))
                        || (event.isAltPressed() && key == KeyEvent.KEYCODE_DPAD_LEFT);
        if (!ours) return false;
        if (event.getAction() != KeyEvent.ACTION_DOWN || event.getRepeatCount() != 0) return true;
        if (key == KeyEvent.KEYCODE_ESCAPE) {
            if (mState.getEditing()) {
                onEditAddress(false);
            } else {
                onClose();
            }
        } else if (key == KeyEvent.KEYCODE_W) {
            onClose();
        } else if (key == KeyEvent.KEYCODE_O) {
            onOpen("");
        } else if (key == KeyEvent.KEYCODE_R) {
            onReload();
        } else if (key == KeyEvent.KEYCODE_L) {
            onEditAddress(true);
        } else {
            onBack();
        }
        return true;
    }

    private void hide() {
        mState.setOpen(false);
        mState.setEditing(false);
        // After the card's slide away (PeekUi), the overlay stops taking touches.
        ThreadUtils.postOnUiThreadDelayed(
                () -> {
                    if (mContents == null) mView.setVisibility(View.GONE);
                },
                PeekUi.CLOSE_MS);
    }

    // Lets go of the card's page view, and of the page unless a tab took it.
    private void letGo(boolean destroy) {
        WebContents contents = mContents;
        mContents = null;
        mPageHost.removeAllViews();
        ThinWebView thinWebView = mThinWebView;
        mThinWebView = null;
        mDelegate = null;
        mContentView = null;
        if (thinWebView != null) thinWebView.destroy();
        if (destroy && contents != null) {
            // After the current event: the page may be handling it (window.close()).
            ThreadUtils.postOnUiThread(contents::destroy);
        }
    }

    public void destroy() {
        letGo(/* destroy= */ true);
    }

    // The card's page: links load in it; one that opens a new tab or window opens a tab of the
    // window, as from the tab, and the card closes.
    private class Delegate extends WebContentsDelegateAndroid {
        @Override
        public void navigationStateChanged(int flags) {
            update();
        }

        @Override
        public void loadingStateChanged(boolean shouldShowLoadingUi) {
            update();
        }

        @Override
        public void visibleSSLStateChanged() {
            update();
        }

        @Override
        public void openNewTab(
                GURL url,
                String extraHeaders,
                @Nullable ResourceRequestBody postData,
                int disposition,
                boolean isRendererInitiated) {
            openInWindow(url);
        }

        @Override
        public boolean shouldCreateWebContents(GURL targetUrl) {
            openInWindow(targetUrl);
            return false;
        }

        @Override
        public void closeContents() {
            onClose();
        }
    }

    private void openInWindow(GURL url) {
        if (!url.isValid()) return;
        mActivity
                .getTabCreator(mIncognito)
                .createNewTab(
                        new LoadUrlParams(url, PageTransition.LINK),
                        TabLaunchType.FROM_LINK,
                        null);
        onClose();
    }
}
