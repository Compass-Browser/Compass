// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.app.Activity;
import android.app.role.RoleManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.R;

import org.chromium.base.ActivityState;
import org.chromium.base.ApplicationStatus;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.preferences.Pref;
import org.chromium.chrome.browser.prefetch.settings.PreloadPagesSettingsBridge;
import org.chromium.chrome.browser.prefetch.settings.PreloadPagesState;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.safe_browsing.SafeBrowsingBridge;
import org.chromium.chrome.browser.search_engines.TemplateUrlServiceFactory;
import org.chromium.chrome.browser.settings.SettingsNavigationFactory;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.components.browser_ui.settings.SettingsNavigation.SettingsFragment;
import org.chromium.components.browser_ui.site_settings.WebsitePreferenceBridge;
import org.chromium.components.content_settings.ContentSettingsType;
import org.chromium.components.search_engines.TemplateUrl;
import org.chromium.components.prefs.PrefService;
import org.chromium.components.search_engines.TemplateUrlService;
import org.chromium.components.user_prefs.UserPrefs;
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
    // Chrome's cookie controls (content_settings::CookieControlsMode, pref_names.h).
    private static final String COOKIE_CONTROLS_MODE = "profile.cookie_controls_mode";
    private static final int COOKIES_BLOCK_THIRD_PARTY = 1;
    private static final int COOKIES_INCOGNITO_ONLY = 2;

    private final ChromeTabbedActivity mActivity;
    private final CompassBar mBar;
    private final CompassSettingsUi.State mState = new CompassSettingsUi.State();
    private final View mView;
    // Follows the ad blocker's lists while its page shows.
    private @Nullable AdBlockBridge mAdBlock;

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
        // Back from Android's settings (the default browser), the row says what's chosen now.
        ApplicationStatus.registerStateListenerForActivity(
                (a, state) -> {
                    if (state == ActivityState.RESUMED && mState.getOpen()) loadDefaultBrowser();
                },
                activity);
    }

    public void show() {
        mState.setPage(CompassSettingsUi.Page.MAIN);
        loadSearchEngines();
        mState.setSyncSummary(mBar.syncSummary());
        mState.setProfiles(mBar.profileRows());
        mState.setOnLaunch(OnLaunch.get());
        mState.setLittleWindow(LittleWindow.isOn());
        mState.setAdBlock(AdBlockBridge.isEnabled());
        loadDefaultBrowser();
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
            case DEFAULT_BROWSER -> chooseDefaultBrowser();
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
            case SITE_SETTINGS -> {
                loadSitePermissions();
                mState.setPage(CompassSettingsUi.Page.SITE_SETTINGS);
            }
            case ALL_SITES -> chromeSettings(SettingsFragment.SITE);
            case SITES_IN_SPACES -> {
                loadLinkRules();
                mState.setPage(CompassSettingsUi.Page.SITES_IN_SPACES);
            }
            case LICENSES -> {
                loadLicenses();
                mState.setPage(CompassSettingsUi.Page.LICENSES);
            }
            case PRIVACY -> {
                loadPrivacy();
                mState.setPage(CompassSettingsUi.Page.PRIVACY);
            }
            case AD_BLOCKER -> {
                if (mAdBlock == null) mAdBlock = new AdBlockBridge(this::loadAdBlock);
                loadAdBlock();
                mState.setPage(CompassSettingsUi.Page.AD_BLOCKER);
            }
            case MORE_PRIVACY -> chromeSettings(SettingsFragment.PRIVACY);
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

    private RoleManager roles() {
        return (RoleManager) mActivity.getSystemService(Context.ROLE_SERVICE);
    }

    private void loadDefaultBrowser() {
        RoleManager roles = roles();
        mState.setDefaultBrowser(roles != null && roles.isRoleHeld(RoleManager.ROLE_BROWSER));
    }

    /**
     * Settings → Default Browser: Android's dialog that makes Compass the browser app, as Chrome's
     * own promo asks (DefaultBrowserPromoManager). Android stops showing it to an app the user
     * turned down twice and answers at once, so then, and when Compass already is the browser
     * app, Android's Default apps settings open instead, where the user picks one.
     */
    private void chooseDefaultBrowser() {
        RoleManager roles = roles();
        if (roles == null
                || !roles.isRoleAvailable(RoleManager.ROLE_BROWSER)
                || roles.isRoleHeld(RoleManager.ROLE_BROWSER)) {
            openDefaultAppsSettings();
            return;
        }
        long asked = android.os.SystemClock.elapsedRealtime();
        boolean shown =
                mActivity
                        .getWindowAndroid()
                        .showIntent(
                                roles.createRequestRoleIntent(RoleManager.ROLE_BROWSER),
                                (resultCode, data) -> {
                                    loadDefaultBrowser();
                                    // An answer this quick isn't the user's: the dialog didn't show.
                                    if (resultCode != Activity.RESULT_OK
                                            && android.os.SystemClock.elapsedRealtime() - asked
                                                    < 500) {
                                        openDefaultAppsSettings();
                                    }
                                },
                                null);
        if (!shown) openDefaultAppsSettings();
    }

    private void openDefaultAppsSettings() {
        try {
            mActivity.startActivity(
                    new Intent(android.provider.Settings.ACTION_MANAGE_DEFAULT_APPS_SETTINGS));
        } catch (android.content.ActivityNotFoundException e) {
            mActivity.startActivity(new Intent(android.provider.Settings.ACTION_SETTINGS));
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

    private Profile profile() {
        return mActivity.getTabModelSelector().getModel(false).getProfile();
    }

    // Privacy's page from Chrome's settings: Safe Browsing's level, and its switches.
    private void loadPrivacy() {
        Profile profile = profile();
        PrefService prefs = UserPrefs.get(profile);
        SafeBrowsingBridge safeBrowsing = new SafeBrowsingBridge(profile);
        mState.setSafeBrowsing(safeBrowsing.getSafeBrowsingState());
        mState.setSafeBrowsingManaged(safeBrowsing.isSafeBrowsingManaged());
        Set<CompassSettingsUi.PrivacySwitch> on = new HashSet<>();
        Set<CompassSettingsUi.PrivacySwitch> managed = new HashSet<>();
        if (prefs.getInteger(COOKIE_CONTROLS_MODE) == COOKIES_BLOCK_THIRD_PARTY) {
            on.add(CompassSettingsUi.PrivacySwitch.BLOCK_THIRD_PARTY_COOKIES);
        }
        if (prefs.isManagedPreference(COOKIE_CONTROLS_MODE)) {
            managed.add(CompassSettingsUi.PrivacySwitch.BLOCK_THIRD_PARTY_COOKIES);
        }
        if (prefs.getBoolean(Pref.HTTPS_ONLY_MODE_ENABLED)) {
            on.add(CompassSettingsUi.PrivacySwitch.HTTPS_ONLY);
        }
        if (prefs.isManagedPreference(Pref.HTTPS_ONLY_MODE_ENABLED)) {
            managed.add(CompassSettingsUi.PrivacySwitch.HTTPS_ONLY);
        }
        if (PreloadPagesSettingsBridge.getState(profile) != PreloadPagesState.NO_PRELOADING) {
            on.add(CompassSettingsUi.PrivacySwitch.PRELOAD_PAGES);
        }
        if (PreloadPagesSettingsBridge.isNetworkPredictionManaged(profile)) {
            managed.add(CompassSettingsUi.PrivacySwitch.PRELOAD_PAGES);
        }
        if (prefs.getBoolean(Pref.ENABLE_DO_NOT_TRACK)) {
            on.add(CompassSettingsUi.PrivacySwitch.DO_NOT_TRACK);
        }
        if (prefs.isManagedPreference(Pref.ENABLE_DO_NOT_TRACK)) {
            managed.add(CompassSettingsUi.PrivacySwitch.DO_NOT_TRACK);
        }
        if (prefs.getBoolean(REMOVE_TRACKING_FROM_COPIED_LINKS)) {
            on.add(CompassSettingsUi.PrivacySwitch.CLEAN_COPIED_LINKS);
        }
        mState.setPrivacyOn(on);
        mState.setPrivacyManaged(managed);
    }

    @Override
    public void onSetSafeBrowsing(int level) {
        SafeBrowsingBridge safeBrowsing = new SafeBrowsingBridge(profile());
        if (safeBrowsing.isSafeBrowsingManaged()) return;
        safeBrowsing.setSafeBrowsingState(level);
        loadPrivacy();
    }

    @Override
    public void onSetPrivacySwitch(CompassSettingsUi.PrivacySwitch which, boolean on) {
        Profile profile = profile();
        PrefService prefs = UserPrefs.get(profile);
        switch (which) {
            case BLOCK_THIRD_PARTY_COOKIES -> {
                if (prefs.isManagedPreference(COOKIE_CONTROLS_MODE)) break;
                // Off is "block in Incognito only", Chrome's default on Android.
                prefs.setInteger(
                        COOKIE_CONTROLS_MODE,
                        on ? COOKIES_BLOCK_THIRD_PARTY : COOKIES_INCOGNITO_ONLY);
            }
            case HTTPS_ONLY -> {
                if (prefs.isManagedPreference(Pref.HTTPS_ONLY_MODE_ENABLED)) break;
                prefs.setBoolean(Pref.HTTPS_ONLY_MODE_ENABLED, on);
            }
            case PRELOAD_PAGES -> {
                if (PreloadPagesSettingsBridge.isNetworkPredictionManaged(profile)) break;
                PreloadPagesSettingsBridge.setState(
                        profile,
                        on
                                ? PreloadPagesState.STANDARD_PRELOADING
                                : PreloadPagesState.NO_PRELOADING);
            }
            case DO_NOT_TRACK -> {
                if (prefs.isManagedPreference(Pref.ENABLE_DO_NOT_TRACK)) break;
                prefs.setBoolean(Pref.ENABLE_DO_NOT_TRACK, on);
            }
            case CLEAN_COPIED_LINKS -> prefs.setBoolean(REMOVE_TRACKING_FROM_COPIED_LINKS, on);
        }
        loadPrivacy();
    }

    // Site Settings' permissions: each one's title, what on means, and its default.
    private static final int ASK = R.string.compass_settings_permission_ask;
    private static final int ALLOWED = R.string.compass_settings_permission_allowed;
    private static final int[][] SITE_PERMISSIONS = {
        {ContentSettingsType.GEOLOCATION, R.string.compass_settings_permission_location, ASK},
        {ContentSettingsType.MEDIASTREAM_CAMERA, R.string.compass_settings_permission_camera, ASK},
        {ContentSettingsType.MEDIASTREAM_MIC, R.string.compass_settings_permission_microphone, ASK},
        {ContentSettingsType.NOTIFICATIONS, R.string.compass_settings_permission_notifications, ASK},
        {ContentSettingsType.POPUPS, R.string.compass_settings_permission_popups, ALLOWED},
        {ContentSettingsType.AUTOMATIC_DOWNLOADS, R.string.compass_settings_permission_automatic_downloads, ASK},
        {ContentSettingsType.SOUND, R.string.compass_settings_permission_sound, ALLOWED},
        {ContentSettingsType.JAVASCRIPT, R.string.compass_settings_permission_javascript, ALLOWED},
        // A playing video goes into picture-in-picture when its tab is left (docs/design/
        // android.md, "Auto picture-in-picture"); Compass allows it by default.
        {ContentSettingsType.AUTO_PICTURE_IN_PICTURE, R.string.compass_settings_permission_auto_pip, ALLOWED},
        // Blocked: the ad blocker is on for sites (docs/design/ad-blocker.md); allowed turns it
        // off for every site of the profile.
        {ContentSettingsType.ADS, R.string.compass_settings_permission_ads, ALLOWED},
    };

    // The desktop's "Remove tracking from copied links"
    // (compass::prefs::kRemoveTrackingFromCopiedLinks; CopyLinkBridge).
    private static final String REMOVE_TRACKING_FROM_COPIED_LINKS = "compass.copy.remove_tracking";

    // The desktop's "Also when you leave the window" (compass::prefs::kPipOnLeavingWindow).
    private static final String PIP_ON_LEAVING_WINDOW = "compass.pip.on_leaving_window";

    private void loadSitePermissions() {
        Profile profile = profile();
        List<CompassSettingsUi.SitePermission> permissions = new ArrayList<>();
        for (int[] row : SITE_PERMISSIONS) {
            int type = row[0];
            permissions.add(
                    new CompassSettingsUi.SitePermission(
                            type,
                            mActivity.getString(row[1]),
                            mActivity.getString(row[2]),
                            WebsitePreferenceBridge.isCategoryEnabled(profile, type),
                            WebsitePreferenceBridge.isContentSettingManaged(profile, type)));
        }
        mState.setSitePermissions(permissions);
        mState.setPipOnLeaving(UserPrefs.get(profile).getBoolean(PIP_ON_LEAVING_WINDOW));
    }

    @Override
    public void onSetPipOnLeaving(boolean on) {
        UserPrefs.get(profile()).setBoolean(PIP_ON_LEAVING_WINDOW, on);
        loadSitePermissions();
    }

    @Override
    public void onSetSitePermission(int type, boolean on) {
        Profile profile = profile();
        if (WebsitePreferenceBridge.isContentSettingManaged(profile, type)) return;
        WebsitePreferenceBridge.setCategoryEnabled(profile, type, on);
        loadSitePermissions();
    }

    // Ad Blocker: the switch and the lists (AdBlockBridge).
    private void loadAdBlock() {
        mState.setAdBlock(AdBlockBridge.isEnabled());
        List<CompassSettingsUi.FilterListRow> rows = new ArrayList<>();
        boolean updating = false;
        try {
            org.json.JSONArray lists = new org.json.JSONArray(AdBlockBridge.listsJson());
            for (int i = 0; i < lists.length(); i++) {
                org.json.JSONObject list = lists.getJSONObject(i);
                updating |= list.optBoolean("updating");
                rows.add(
                        new CompassSettingsUi.FilterListRow(
                                list.optString("id"),
                                list.optString("title"),
                                list.optBoolean("builtin"),
                                list.optBoolean("enabled"),
                                filterListDetail(list),
                                list.optDouble("updatedMs") > 0 ? list.optString("error") : ""));
            }
        } catch (org.json.JSONException e) {
            // None, then.
        }
        mState.setFilterLists(rows);
        mState.setFilterListsUpdating(updating);
    }

    // "12,345 rules · updated 2 hours ago", or why there are none yet.
    private String filterListDetail(org.json.JSONObject list) {
        if (list.optBoolean("updating")) {
            return mActivity.getString(R.string.compass_settings_filter_lists_updating);
        }
        double updated = list.optDouble("updatedMs");
        if (!(updated > 0)) {
            String error = list.optString("error");
            // The error comes from the native side (AdBlockBridge), in English.
            return error.isEmpty()
                    ? mActivity.getString(R.string.compass_settings_filter_list_not_downloaded)
                    : error;
        }
        int rules = list.optInt("urlRules") + list.optInt("cssRules");
        CharSequence ago =
                android.text.format.DateUtils.getRelativeTimeSpanString(
                        (long) updated,
                        System.currentTimeMillis(),
                        android.text.format.DateUtils.MINUTE_IN_MILLIS);
        return mActivity.getResources()
                .getQuantityString(
                        R.plurals.compass_settings_filter_list_rules,
                        rules,
                        String.format(java.util.Locale.getDefault(), "%,d", rules),
                        ago);
    }

    @Override
    public void onSetAdBlock(boolean on) {
        AdBlockBridge.setEnabled(on);
        loadAdBlock();
    }

    @Override
    public void onSetFilterList(String id, boolean on) {
        AdBlockBridge.setListEnabled(id, on);
        loadAdBlock();
    }

    @Override
    public String onAddFilterList(String url) {
        String error = AdBlockBridge.addList(url);
        loadAdBlock();
        return error;
    }

    @Override
    public void onRemoveFilterList(String id) {
        AdBlockBridge.removeList(id);
        loadAdBlock();
    }

    @Override
    public void onUpdateFilterLists() {
        AdBlockBridge.updateNow();
        loadAdBlock();
    }

    private void stopFollowingAdBlock() {
        if (mAdBlock == null) return;
        mAdBlock.destroy();
        mAdBlock = null;
    }

    @Override
    public void onBack() {
        if (mState.getPage() == CompassSettingsUi.Page.AD_BLOCKER) stopFollowingAdBlock();
        mState.setPage(CompassSettingsUi.Page.MAIN);
    }

    @Override
    public void onSetOnLaunch(String value) {
        OnLaunch.set(value);
        mState.setOnLaunch(OnLaunch.get());
    }

    // Third-Party Licenses: the license files Compass adds, from the APK's assets
    // (android/BUILD.gn :license_assets).
    private static final int[] LICENSE_TITLES = {
        R.string.compass_settings_license_mpl2_title, R.string.compass_settings_license_lgpl_title,
    };
    private static final int[] LICENSE_ABOUTS = {
        R.string.compass_settings_license_mpl2_about, R.string.compass_settings_license_lgpl_about,
    };
    private static final String[] LICENSE_FILES = {
        "licenses/strip_on_share_mpl2.txt", "licenses/strip_on_share_lgpl.txt",
    };
    private static final String LICENSES_SOURCE =
            "https://github.com/mozilla-firefox/firefox,"
                    + " toolkit/components/antitracking/StripOnShareLists";

    private void loadLicenses() {
        List<CompassSettingsUi.License> licenses = new ArrayList<>();
        for (int i = 0; i < LICENSE_FILES.length; i++) {
            String text;
            try (java.io.InputStream in = mActivity.getAssets().open(LICENSE_FILES[i])) {
                java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                byte[] buffer = new byte[8192];
                for (int n; (n = in.read(buffer)) != -1; ) bytes.write(buffer, 0, n);
                text = bytes.toString("UTF-8");
            } catch (java.io.IOException e) {
                text = mActivity.getString(R.string.compass_settings_license_missing);
            }
            licenses.add(
                    new CompassSettingsUi.License(
                            mActivity.getString(LICENSE_TITLES[i]),
                            mActivity.getString(
                                    R.string.compass_settings_license_about,
                                    mActivity.getString(LICENSE_ABOUTS[i]),
                                    LICENSES_SOURCE),
                            text));
        }
        mState.setLicenses(licenses);
    }

    // Sites in Spaces (link-routing.md): this window's profile's rules and spaces.
    private void loadLinkRules() {
        Spaces spaces = mBar.spaces();
        if (spaces == null) return;
        java.util.Map<String, CompassSettingsUi.SpaceChoice> byId = new java.util.HashMap<>();
        List<CompassSettingsUi.SpaceChoice> choices = new ArrayList<>();
        for (SidebarState.Space space : spaces.state().getSpaces()) {
            CompassSettingsUi.SpaceChoice choice =
                    new CompassSettingsUi.SpaceChoice(space.id, space.name, space.emoji, space.tint);
            choices.add(choice);
            byId.put(space.id, choice);
        }
        List<CompassSettingsUi.LinkRuleRow> rules = new ArrayList<>();
        try {
            org.json.JSONArray array = new org.json.JSONArray(spaces.bridge().linkRules());
            for (int i = 0; i < array.length(); i++) {
                org.json.JSONObject rule = array.getJSONObject(i);
                rules.add(
                        new CompassSettingsUi.LinkRuleRow(
                                rule.optString("domain"), byId.get(rule.optString("space"))));
            }
        } catch (org.json.JSONException e) {
            // None, then.
        }
        mState.setRuleSpaces(choices);
        mState.setLinkRules(rules);
    }

    @Override
    public String normalizeLinkDomain(String input) {
        Spaces spaces = mBar.spaces();
        return spaces == null ? "" : spaces.bridge().normalizeLinkDomain(input);
    }

    @Override
    public void onSetLinkRule(String domain, String spaceId) {
        Spaces spaces = mBar.spaces();
        if (spaces != null) spaces.bridge().setLinkRule(domain, spaceId);
        loadLinkRules();
    }

    @Override
    public void onRemoveLinkRule(String domain) {
        Spaces spaces = mBar.spaces();
        if (spaces != null) spaces.bridge().removeLinkRule(domain);
        loadLinkRules();
    }

    @Override
    public void onSetLittleWindow(boolean on) {
        LittleWindow.setOn(on);
        mState.setLittleWindow(LittleWindow.isOn());
    }

    @Override
    public void onDismiss() {
        stopFollowingAdBlock();
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
