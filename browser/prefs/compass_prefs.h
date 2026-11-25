// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_PREFS_COMPASS_PREFS_H_
#define COMPASS_BROWSER_PREFS_COMPASS_PREFS_H_

class PrefRegistrySimple;
class PrefService;

namespace compass {

namespace prefs {

// A profile imported from Arc: Arc's profile folder it came from ("Default",
// "Profile 1", ...). Written with the copied preferences, before the profile
// first loads (docs/design/arc-import.md).
inline constexpr char kArcImportProfileDir[] =
    "compass.arc_import.profile_dir";

// The Arc space IDs imported into this profile, so importing again only
// offers Arc's newer spaces (docs/design/arc-import.md, "Importing again").
inline constexpr char kArcImportSpaces[] = "compass.arc_import.spaces";

// A profile copied from another Chromium-based browser's profile: its ID
// ("chrome:Default", browser_import::SourceProfile::id). Written with the
// copied preferences, before the profile first loads
// (docs/design/browser-import.md, "As a profile").
inline constexpr char kBrowserImportProfile[] =
    "compass.browser_import.profile";

// Whether a playing video (or a call) goes into picture-in-picture when its
// window is covered or minimized, or Compass is left for another app, as it
// does when its tab is left (docs/design/picture-in-picture.md). Default on.
inline constexpr char kPipOnLeavingWindow[] = "compass.pip.on_leaving_window";

// Whether copying the page's link (Copy URL, the sidebar's Copy Link, and
// Android's Copy URL) leaves out tracking parameters
// (compass::RemoveTrackingParams(); docs/design/shortcuts.md "Clean
// links"). Default on.
inline constexpr char kRemoveTrackingFromCopiedLinks[] =
    "compass.copy.remove_tracking";

// The sites in development mode (docs/design/dev-mode.md): host patterns
// as compass::dev_mode::NormalizeHost() writes them ("localhost",
// "example.com", "*.example.com"). Not synced: a developer's sites belong
// to the machine they work on.
inline constexpr char kDevModeHosts[] = "compass.dev_mode.hosts";
// Sites (registrable domains, e.g. "example.com") that get a Safari user
// agent (docs/design/fairplay.md, "Present as Safari"). macOS.
inline constexpr char kPresentAsSafariSites[] =
    "compass.present_as_safari.sites";
// kDevModeHosts' default: local development servers at "localhost".
// ("*.localhost" isn't a pattern BoostMatch takes, a single label being too
// wide; 127.0.0.1 is left out because the browser tests' server is there.)
inline constexpr char kDevModeDefaultHost[] = "localhost";

// Whether Ctrl+Tab goes through the window's tabs in recently used order,
// with a row of cards while Ctrl is held, as Arc does (docs/design/sidebar.md,
// "Recent tabs"); off, it's Next tab in sidebar order. Default on.
inline constexpr char kCtrlTabRecent[] = "compass.tabs.ctrl_tab_recent";

// Whether sidebar hover cards show plugins' tab previews
// (docs/design/live-folders.md, "Tab previews"). Default on.
inline constexpr char kTabPreviewsEnabled[] = "compass.tab_previews.enabled";

// Each preview plugin's settings: {<plugin id>: {"enabled": bool, "config":
// {...}, "session_granted": bool, "session_hosts": [...]}}. Built-ins are on
// unless turned off; installed plugins are off until turned on. A session
// grant is for the related hosts it lists, so an update adding hosts asks
// again. Not synced: the grant is for this device's sessions.
inline constexpr char kTabPreviewPlugins[] = "compass.tab_previews.plugins";

}  // namespace prefs

// Compass's own profile prefs. Called by upstream's RegisterProfilePrefs()
// (patches/chrome-browser-prefs-browser_prefs.cc.patch).
void RegisterCompassProfilePrefs(PrefRegistrySimple* registry);

// Moves Compass's profile prefs whose names changed. Called by upstream's
// MigrateObsoleteProfilePrefs() (see
// patches/chrome-browser-prefs-browser_prefs.cc.patch) as a profile loads.
void MigrateCompassProfilePrefs(PrefService* prefs);

}  // namespace compass

#endif  // COMPASS_BROWSER_PREFS_COMPASS_PREFS_H_
