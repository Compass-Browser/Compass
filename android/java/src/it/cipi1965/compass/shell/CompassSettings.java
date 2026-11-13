// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.content.Intent;
import android.net.Uri;
import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.search_engines.TemplateUrlServiceFactory;
import org.chromium.chrome.browser.settings.SettingsNavigationFactory;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.components.browser_ui.settings.SettingsNavigation.SettingsFragment;
import org.chromium.components.search_engines.TemplateUrl;
import org.chromium.components.search_engines.TemplateUrlService;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.ui.base.PageTransition;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Compass's settings screen (CompassSettingsUi.kt), from the sidebar's gear and the page menu.
 * Its rows open Compass's own screens (through CompassBar) or Chrome's settings pages.
 */
public class CompassSettings implements CompassSettingsUi.Listener {
    private static final String PRIVACY_POLICY_URL = "https://compassbrowser.org/privacy";

    private final ChromeTabbedActivity mActivity;
    private final CompassBar mBar;
    private final CompassSettingsUi.State mState = new CompassSettingsUi.State();
    private final View mView;

    public CompassSettings(ChromeTabbedActivity activity, ViewGroup parent, CompassBar bar) {
        mActivity = activity;
        mBar = bar;
        mView = CompassSettingsUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        try {
            mState.setVersion(
                    activity.getPackageManager()
                            .getPackageInfo(activity.getPackageName(), 0)
                            .versionName);
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            // No version line.
        }
    }

    public void show() {
        mState.setPage(CompassSettingsUi.Page.MAIN);
        loadSearchEngines();
        mState.setSyncSummary(mBar.syncSummary());
        mState.setOnLaunch(OnLaunch.get());
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
    }

    private void chromeSettings(@SettingsFragment int fragment) {
        SettingsNavigationFactory.createSettingsNavigation().startSettings(mActivity, fragment);
    }

    @Override
    public void onRow(CompassSettingsUi.Row row) {
        switch (row) {
            case SYNC -> {
                onDismiss();
                mBar.showSync();
            }
            case SEARCH_ENGINE -> {
                loadSearchEngines();
                mState.setPage(CompassSettingsUi.Page.SEARCH_ENGINE);
            }
            case PASSWORDS -> chromeSettings(SettingsFragment.AUTOFILL_AND_PASSWORDS);
            case AUTOFILL_APPS -> openAutofillSettings();
            case CLAUDE -> {
                onDismiss();
                mBar.showClaude();
            }
            case EXTENSIONS -> {
                onDismiss();
                mBar.showExtensions();
            }
            case CLEAR_DATA -> chromeSettings(SettingsFragment.CLEAR_BROWSING_DATA);
            case DOWNLOADS -> chromeSettings(SettingsFragment.DOWNLOADS);
            case SITE_SETTINGS -> chromeSettings(SettingsFragment.SITE);
            case PRIVACY -> chromeSettings(SettingsFragment.PRIVACY);
            case ACCESSIBILITY -> chromeSettings(SettingsFragment.ACCESSIBILITY);
            case LANGUAGES -> chromeSettings(SettingsFragment.LANGUAGE);
            case MORE -> chromeSettings(SettingsFragment.MAIN);
            case PRIVACY_POLICY -> {
                onDismiss();
                mActivity
                        .getTabCreator(false)
                        .createNewTab(
                                new LoadUrlParams(PRIVACY_POLICY_URL, PageTransition.AUTO_TOPLEVEL),
                                TabLaunchType.FROM_CHROME_UI,
                                mActivity.getActivityTab());
            }
        }
    }

    /**
     * Android's setting for the app that fills passwords in other apps: it asks to make Compass
     * that app (CompassAutofillService); if Compass already is, its own settings.
     */
    private void openAutofillSettings() {
        Intent intent =
                new Intent(android.provider.Settings.ACTION_REQUEST_SET_AUTOFILL_SERVICE)
                        .setData(Uri.parse("package:" + mActivity.getPackageName()));
        try {
            mActivity.startActivity(intent);
        } catch (android.content.ActivityNotFoundException e) {
            mActivity.startActivity(new Intent(android.provider.Settings.ACTION_SETTINGS));
        }
    }

    private TemplateUrlService searchEngines() {
        return TemplateUrlServiceFactory.getForProfile(
                mActivity.getTabModelSelector().getModel(false).getProfile());
    }

    // The engines Chrome offers (its prepopulated ones, and the default if it's another one) and
    // the default, once Chrome's list is loaded.
    private void loadSearchEngines() {
        TemplateUrlService service = searchEngines();
        service.runWhenLoaded(
                () -> {
                    TemplateUrl current = service.getDefaultSearchEngineTemplateUrl();
                    List<CompassSettingsUi.Engine> engines = new ArrayList<>();
                    Set<String> keywords = new HashSet<>();
                    for (TemplateUrl url : service.getTemplateUrls()) {
                        boolean isCurrent =
                                current != null && url.getKeyword().equals(current.getKeyword());
                        if (!url.getIsPrepopulated() && !isCurrent) continue;
                        if (!keywords.add(url.getKeyword())) continue;
                        engines.add(
                                new CompassSettingsUi.Engine(
                                        url.getKeyword(), url.getShortName(), host(url.getURL())));
                    }
                    mState.setSearchEngines(engines);
                    mState.setSearchEngine(current != null ? current.getKeyword() : "");
                    mState.setSearchEngineManaged(service.isDefaultSearchManaged());
                });
    }

    // A search URL template's host ("www.google.com"), or empty.
    private static String host(String template) {
        String host = Uri.parse(template.replace("{searchTerms}", "q")).getHost();
        return host != null ? host : "";
    }

    @Override
    public void onSetSearchEngine(String keyword) {
        TemplateUrlService service = searchEngines();
        if (service.isDefaultSearchManaged()) return;
        service.setSearchEngine(keyword);
        mState.setSearchEngine(keyword);
    }

    @Override
    public void onBack() {
        mState.setPage(CompassSettingsUi.Page.MAIN);
    }

    @Override
    public void onSetOnLaunch(String value) {
        OnLaunch.set(value);
        mState.setOnLaunch(OnLaunch.get());
    }

    @Override
    public void onDismiss() {
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
