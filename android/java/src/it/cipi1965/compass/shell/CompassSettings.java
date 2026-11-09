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
import org.chromium.chrome.browser.settings.SettingsNavigationFactory;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.components.browser_ui.settings.SettingsNavigation.SettingsFragment;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.ui.base.PageTransition;

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
        mState.setSyncSummary(mBar.syncSummary());
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
            case SEARCH_ENGINE -> chromeSettings(SettingsFragment.SEARCH_ENGINE);
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

    @Override
    public void onDismiss() {
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
