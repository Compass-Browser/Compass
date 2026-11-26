// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.StringRes;
import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.R;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.settings.SettingsNavigationFactory;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.components.browser_ui.site_settings.SingleWebsiteSettings;
import org.chromium.components.browser_ui.site_settings.WebsitePreferenceBridge;
import org.chromium.components.content_settings.ContentSetting;
import org.chromium.components.content_settings.ContentSettingsType;
import org.chromium.components.security_state.ConnectionSecurityLevel;
import org.chromium.components.security_state.SecurityStateModel;
import org.chromium.content_public.browser.WebContents;
import org.chromium.url.GURL;

import java.util.ArrayList;
import java.util.List;

/**
 * The Site sheet (SiteSheetUi.kt; docs/design/android.md, "The Site sheet"), Compass's in place of
 * Chrome's page info, which drops from the top toolbar Compass doesn't have: the shown page's
 * site, its connection, the permissions it was given or denied (each a switch, and a reset), its
 * own settings (the ad blocker, desktop site, sound, pop-ups, automatic picture-in-picture,
 * JavaScript), and Chrome's page for the rest.
 */
public class SiteSheet implements SiteSheetUi.Listener {
    /** A permission the sheet lists when the site has its own setting for it. */
    private static final class Permission {
        final int mType;
        final @StringRes int mTitle;
        final int mIcon;

        Permission(int type, @StringRes int title, int icon) {
            mType = type;
            mTitle = title;
            mIcon = icon;
        }

        int type() {
            return mType;
        }

        @StringRes
        int title() {
            return mTitle;
        }

        int icon() {
            return mIcon;
        }
    }

    private static final Permission[] PERMISSIONS = {
        new Permission(ContentSettingsType.GEOLOCATION, R.string.compass_site_location, R.drawable.compass_sym_location_on),
        new Permission(ContentSettingsType.MEDIASTREAM_CAMERA, R.string.compass_site_camera, R.drawable.compass_sym_videocam),
        new Permission(ContentSettingsType.MEDIASTREAM_MIC, R.string.compass_site_microphone, R.drawable.compass_sym_mic),
        new Permission(
                ContentSettingsType.NOTIFICATIONS, R.string.compass_site_notifications, R.drawable.compass_sym_notifications),
        new Permission(
                ContentSettingsType.CLIPBOARD_READ_WRITE, R.string.compass_site_clipboard, R.drawable.compass_sym_content_paste),
        new Permission(ContentSettingsType.SENSORS, R.string.compass_site_motion_sensors, R.drawable.compass_sym_sensors),
        new Permission(ContentSettingsType.MIDI_SYSEX, R.string.compass_site_midi_devices, R.drawable.compass_sym_piano),
        new Permission(ContentSettingsType.NFC, R.string.compass_site_nfc_devices, R.drawable.compass_sym_nfc),
    };

    /**
     * The ad blocker's switch for the site (docs/design/ad-blocker.md): on blocks, which is the
     * Ads site setting's block (its default); off allows ads there. Listed while the ad blocker is
     * on.
     */
    private static final Permission AD_BLOCKER =
            new Permission(ContentSettingsType.ADS, R.string.compass_site_ad_blocker, R.drawable.compass_sym_shield);

    /** The site's own settings: always listed. */
    private static final Permission[] OPTIONS = {
        new Permission(
                ContentSettingsType.REQUEST_DESKTOP_SITE,
                R.string.compass_site_desktop_site,
                R.drawable.compass_sym_desktop_windows),
        new Permission(ContentSettingsType.SOUND, R.string.compass_site_sound, R.drawable.compass_sym_volume_up),
        new Permission(
                ContentSettingsType.POPUPS, R.string.compass_site_popups, R.drawable.compass_sym_open_in_new),
        new Permission(
                ContentSettingsType.AUTO_PICTURE_IN_PICTURE,
                R.string.compass_site_auto_pip,
                R.drawable.compass_sym_picture_in_picture),
        new Permission(ContentSettingsType.JAVASCRIPT, R.string.compass_site_javascript, R.drawable.compass_sym_javascript),
    };

    private final ChromeTabbedActivity mActivity;
    private final SiteSheetState mState = new SiteSheetState();
    private final View mView;
    // While it shows: the tab's profile and page, and the type behind each switch's key.
    private @Nullable Profile mProfile;
    private @Nullable Tab mTab;
    private GURL mUrl = GURL.emptyGURL();
    private final List<Permission> mKeys = new ArrayList<>();

    public SiteSheet(ChromeTabbedActivity activity, ViewGroup parent) {
        mActivity = activity;
        mView = SiteSheetUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    /** Shows |tab|'s site; false for a page without one (Chrome's page info then). */
    public boolean show(Tab tab) {
        GURL url = tab.getUrl();
        if (!url.getScheme().equals("http") && !url.getScheme().equals("https")) return false;
        mTab = tab;
        mProfile = tab.getProfile();
        mUrl = url;
        mState.setHost(url.getHost());
        mState.setUrl(url.getSpec());
        mState.setConnection(connection(tab.getWebContents()));
        refresh();
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
        return true;
    }

    public void destroy() {
        ((ViewGroup) mView.getParent()).removeView(mView);
    }

    private void refresh() {
        Profile profile = mProfile;
        if (profile == null) return;
        mKeys.clear();
        String origin = origin();
        List<SiteSheetState.Setting> permissions = new ArrayList<>();
        for (Permission permission : PERMISSIONS) {
            int setting = permissionSetting(profile, permission.type(), origin);
            int fallback = WebsitePreferenceBridge.getDefaultContentSetting(profile, permission.type());
            // Only the site's own decisions: an "ask" (the default) isn't listed.
            if (setting == fallback || setting == ContentSetting.ASK) continue;
            permissions.add(setting(permission, setting == ContentSetting.ALLOW, true));
        }
        List<SiteSheetState.Setting> options = new ArrayList<>();
        if (AdBlockBridge.isEnabled()) {
            int ads = WebsitePreferenceBridge.getContentSetting(profile, AD_BLOCKER.type(), mUrl, mUrl);
            options.add(
                    setting(
                            AD_BLOCKER,
                            ads != ContentSetting.ALLOW,
                            WebsitePreferenceBridge.isContentSettingUserModifiable(
                                    profile, AD_BLOCKER.type())));
        }
        for (Permission option : OPTIONS) {
            int setting =
                    WebsitePreferenceBridge.getContentSetting(profile, option.type(), mUrl, mUrl);
            boolean modifiable =
                    WebsitePreferenceBridge.isContentSettingUserModifiable(profile, option.type());
            options.add(setting(option, setting == ContentSetting.ALLOW, modifiable));
        }
        mState.setPermissions(permissions);
        mState.setOptions(options);
    }

    private SiteSheetState.Setting setting(Permission permission, boolean on, boolean enabled) {
        mKeys.add(permission);
        boolean onOff =
                permission.type() == ContentSettingsType.REQUEST_DESKTOP_SITE
                        || permission == AD_BLOCKER;
        return new SiteSheetState.Setting(
                mKeys.size() - 1,
                mActivity.getString(permission.title()),
                permission.icon(),
                on,
                enabled,
                mActivity.getString(onOff ? R.string.compass_on : R.string.compass_site_allowed),
                mActivity.getString(onOff ? R.string.compass_off : R.string.compass_site_blocked));
    }

    private String origin() {
        return mUrl.getScheme() + "://" + mUrl.getHost() + (mUrl.getPort().isEmpty() ? "" : ":" + mUrl.getPort());
    }

    private static int permissionSetting(Profile profile, int type, String origin) {
        try {
            Integer value =
                    WebsitePreferenceBridge.getPermissionSettingWithEmbargo(
                                    profile, type, origin, origin)
                            .getContentSetting();
            return value != null ? value : ContentSetting.DEFAULT;
        } catch (RuntimeException e) {
            return ContentSetting.DEFAULT;
        }
    }

    private static SiteSheetState.Connection connection(@Nullable WebContents contents) {
        if (contents == null) return SiteSheetState.Connection.NONE;
        int level = SecurityStateModel.getSecurityLevelForWebContents(contents);
        if (level == ConnectionSecurityLevel.SECURE) return SiteSheetState.Connection.SECURE;
        if (level == ConnectionSecurityLevel.DANGEROUS) return SiteSheetState.Connection.DANGEROUS;
        return SiteSheetState.Connection.NOT_SECURE;
    }

    // SiteSheetUi.Listener:

    @Override
    public void onToggle(int key, boolean on) {
        Profile profile = mProfile;
        if (profile == null || key < 0 || key >= mKeys.size()) return;
        Permission permission = mKeys.get(key);
        if (permission == AD_BLOCKER) {
            // On is the default (blocked); off allows ads on the site. The page's requests went
            // out with the old choice, so it reloads.
            WebsitePreferenceBridge.setContentSettingDefaultScope(
                    profile,
                    ContentSettingsType.ADS,
                    mUrl,
                    mUrl,
                    on ? ContentSetting.DEFAULT : ContentSetting.ALLOW);
            if (mTab != null) mTab.reload();
            refresh();
            return;
        }
        int value = on ? ContentSetting.ALLOW : ContentSetting.BLOCK;
        boolean isOption = false;
        for (Permission option : OPTIONS) isOption |= option == permission;
        if (isOption) {
            WebsitePreferenceBridge.setContentSettingDefaultScope(
                    profile, permission.type(), mUrl, mUrl, value);
            // Desktop site and JavaScript take effect on a reload, as Chrome's page info does.
            if (mTab != null
                    && (permission.type() == ContentSettingsType.REQUEST_DESKTOP_SITE
                            || permission.type() == ContentSettingsType.JAVASCRIPT)) {
                mTab.reload();
            }
        } else {
            String origin = origin();
            WebsitePreferenceBridge.setPermissionSettingForOrigin(
                    profile, permission.type(), origin, origin, value);
        }
        refresh();
    }

    @Override
    public void onResetPermissions() {
        Profile profile = mProfile;
        if (profile == null) return;
        String origin = origin();
        for (Permission permission : PERMISSIONS) {
            WebsitePreferenceBridge.setPermissionSettingForOrigin(
                    profile, permission.type(), origin, origin, ContentSetting.DEFAULT);
        }
        refresh();
    }

    @Override
    public void onAllSettings() {
        String url = mUrl.getSpec();
        onDismiss();
        SettingsNavigationFactory.createSettingsNavigation()
                .startSettings(
                        mActivity,
                        SingleWebsiteSettings.class,
                        SingleWebsiteSettings.createFragmentArgsForSite(url));
    }

    @Override
    public void onDismiss() {
        mState.setOpen(false);
        mTab = null;
        mProfile = null;
        mView.postDelayed(
                () -> {
                    if (!mState.getOpen()) mView.setVisibility(View.GONE);
                },
                400);
    }
}
