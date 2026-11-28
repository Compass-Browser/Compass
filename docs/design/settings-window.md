# Settings window

Status: done (phases 1–5). Every section is the window's, and upstream's
settings page is never shown.
Based on Chromium 157.0.8103.51.

## Goal

Settings in a window of their own, laid out like the system's settings
windows (a toolbar of panes) and looking like Compass rather than Chrome:
no Material menu, cards or Google-blue controls. In the end every section is
the window's, Chrome's included, and nothing shows upstream's settings page.

## Using it

- **Cmd+,** (Ctrl+, elsewhere), the app menu's Settings and the command bar's
  settings commands open the profile's settings window, or bring it to the
  front. There's one per profile. It isn't a browser window, so it isn't in
  the sidebar, the space switcher, window sync or session restore.
- Typing or following any `chrome://settings/…` (or `compass://settings/…`)
  or `chrome://compass-settings/…` URL in a tab opens the window at the pane
  for it instead. A tab that was opened only for it closes.
- **Toolbar:** the panes as icon tabs under the title, which names the pane.
  ←/→ move between tabs, and the window opens at the pane shown last.
  - General
  - Profiles: the profile's name and picture, then rows for Sync and Import
    from Another Browser, each opening a page of its own
  - Links: Links from Other Apps
  - Shortcuts
  - Boosts
  - Plugins
  - AI: Claude's settings
  - Privacy
  - Passwords
  - Advanced: performance, accessibility, reset, and a row for Development
    Mode
  - About: Compass's icon, version, updates and update channel
- **Search** (Cmd+F) is at the title row's right. It lists the matching
  sections and rows across panes, and picking one (or Enter for the first)
  goes there and highlights it.
- The window is as tall as the pane needs, up to 85% of the screen; a taller
  pane scrolls. It animates between panes on macOS. **Esc** (with nothing
  focused) and **Cmd+W** close it.
- Links in it (Learn more, a page an upstream row opens) open as tabs in the
  last active window.

## Phases

1. **Done:** the window, the routing of every way into it, the toolbar,
   search, and Compass's sections re-skinned. Upstream's sections were rows
   that opened `chrome://settings/<section>` in a tab, until the phases
   below rebuilt them.
2. **Done:** General and Profiles rebuilt (below, "General and Profiles").
   Managing search engines and languages are views of General (phase 5).
3. **Done:** Privacy rebuilt (below, "Privacy").
4. **Done:** Passwords (below, "Passwords"). Adding and editing passwords,
   cards and addresses, the checkup, passkeys' details and import/export
   open the password manager or upstream's sections.
5. **Done:** Advanced, managing search engines and languages, the card and
   address editors, Safety check (below). Then no more fallback (5f): every
   `chrome://settings` URL opens the window (`SettingsWindowPathFor()` maps
   each sub-page to its pane, or the closest one, or the window as it was),
   and the patches that put Compass's sections and settings into
   chrome://settings went (its menu, routes, views and BUILD.gn,
   `settings_ui`, the About, appearance, cookies and automatic
   picture-in-picture pages, `settings_localized_strings_provider` and
   `prefs_util.cc`'s allowlist). Their two Compass settings moved into the
   window: picture-in-picture when you leave the window (General) and
   removing tracking from copied links (Privacy). The password manager
   (chrome://password-manager) is a page of its own and stays.

Upstream's settings page is built as upstream ships it, without Compass's
sections, and never shown.

## How

- **Window** (`browser/ui/views/settings_window/settings_window.{h,cc}`): a
  `views::Widget` (TYPE_WINDOW, not resizable) whose contents are a
  `views::WebView` over a WebContents it owns, loading
  `chrome://compass-settings/<path>`. One per profile (a map from profile to
  window); deletes itself when the widget goes, and closes with its profile.
  - On macOS `SettingsNativeWidgetMac` (`settings_window_mac.mm`) asks for the
    browser window class with a full-size content view, a transparent title
    bar and no title. `GetWindowFrameTitlebarHeight()` centers the traffic
    lights on the page's title row (30 px). Elsewhere it's a normal titled
    window, with the page under the system's title bar.
  - The page's toolbar is `-webkit-app-region: drag` (its tabs and search
    field `no-drag`). The WebContents supports draggable regions, and the
    client view's `NonClientHitTest()` answers HTCAPTION in them.
  - `SetContentHeight()` takes the page's height plus the frame, within 320
    px and 85% of the screen's work area, keeping the top where it is (or
    moving up to stay on the screen). On macOS it's `-[NSWindow
    setFrame:display:animate:]`.
  - As the WebContents' delegate, it opens the page's links out of
    chrome://compass-settings (`OpenURLFromTab()`, `AddNewContents()`) as
    tabs, and sends unhandled keys to the widget's focus manager.
- **Routing:**
  - `compass::MaybeShowSettingsWindow()` (`settings_window_hooks.h`, declared
    in `:app_menu_hooks`) is asked first by upstream's
    `chrome::ShowSettingsSubPageInTabbedBrowser()`
    (`patches/chrome-browser-ui-chrome_pages.cc.patch`). Every way of opening
    settings ends there. It takes the sub-pages `SettingsWindowPathFor()`
    knows: "" and Compass's sections at their chrome://settings paths (`sync`,
    `importArc`, `links`, `shortcuts`, `boosts`, `plugins`, `devMode`, and
    `claude`, which is the AI pane), with their queries (`boosts?add=`,
    `links?add=`), and every other upstream sub-page (phase 5f).
  - `SettingsWindowNavigationThrottle`, added by upstream's
    `CreateAndAddChromeThrottlesForNavigation()`
    (`patches/chrome-browser-chrome_content_browser_client_navigation_throttles.cc.patch`),
    covers URLs in tabs. It cancels a primary main frame navigation to such a
    chrome://settings URL, or to chrome://compass-settings (except its
    `.html` files: the standalone sections tests load), opens the window, and
    closes the tab if it had committed nothing.
- **Page** (`chrome://compass-settings`, `CompassSettingsUI`):
  - Every path that isn't a file serves `settings_window.html` (the data
    source's default resource), and the page shows the pane the path names
    (`settings_window/panes.ts`, `resolvePath()`).
  - The standalone keyboard shortcuts page moved to `/shortcuts.html`;
    `/boosts.html` stays.
  - The page's interfaces are `sidebar.mojom` `SettingsWindowHandlerFactory`
    (`SettingsWindowHandler`) plus the sections' (`SettingsHandler`,
    `ArcImportHandler`, `compass_sync.mojom` `PageHandlerFactory`,
    `claude.mojom` `SettingsHandler`), whose implementations are
    `SettingsSections::CreateHandlers()`'s.
    Each section binds its own `SettingsHandler` (shortcuts, links, Boosts,
    plugins, development mode), and they all stay bound.
  - `SettingsWindowHandler` (`browser/ui/webui/settings/`) gives the accent
    (the profile's active space's color, or null for grey) and pushes it as
    the space changes. It sizes the window, opens chrome:// pages
    (`OpenUrl()`: the password manager, chrome://extensions, the
    certificate manager, the licenses) in tabs, and
    closes the window. `ShowPath()` tells the page to show a path when the
    window is opened at one while it's open. Before the page has connected,
    the window loads the path's URL instead.
- **Front end** (`resources/sidebar/settings_window/`):
  - `app.ts`/`app.html.ts`: `<compass-settings-window>` (Lit, rendered into
    the light DOM).
  - `panes.ts`: the panes (with their header's color and blurb), their
    sections and rows.
  - `icons.ts`: the line icons, `<compass-settings-icon>`.
  - `window.css`: a static stylesheet.
  - Compass's sections are the elements chrome://settings shows
    (`settings/*_page.ts`). Each is made the first time its pane shows and
    kept, so its state and its Mojo pipe survive switching panes. A path with
    a query makes the pane's sections again, since they read the query when
    made.
  - The last pane is kept in `localStorage`.
  - The page reports its toolbar's height plus its content's (a
    `ResizeObserver`).
  - Esc clears the search, else closes the window when focus isn't in a
    section. Cmd+W closes it; Cmd+F focuses search.

## Pages and views

Long panes were split (2026-11-25) so a pane's main page holds its switches
and short rows, and anything long or rarely used is one row away, behind a ›.

- **A section's page** (the shell's, `app.ts`): a `PaneSection` with
  `subpage` (`panes.ts`: its group's caption, label, tile icon and color)
  isn't shown in the pane. The pane lists it as a row instead, after its own
  groups (`#subpageRows`), and its path (`sync`, `importArc`, `devMode`, with
  any query) shows it alone, under "‹ <pane>" and its name. Old paths keep
  working: chrome://settings/importArc opens the Import page.
  - All of a pane's sections are in the page, hidden but for those shown, so
    a section with a page of its own can tell the pane its status: it
    dispatches `subpage-detail` (bubbling, on itself) with a short string,
    which becomes its row's detail ("On · 3 devices", "2 sites").
- **A pane's views** (inside a section): Privacy (clear, permissions, sites,
  safety, filter lists), General (search engines, languages, fonts),
  Passwords (saved passwords, cards, addresses), AI (connection, Claude in
  the sidebar, files, memory) and Import (a browser, Arc). Lit panes keep a
  `view_` and draw their own "‹ back"; Compass's older sections use
  `CompassSettingsPage.setUpViews()` (`settings/page_base.ts`), which shows
  one `[data-page-view]` block at a time and adds its back link and title.
  - Either way the path says the view (`?view=`) and the element has the
    `nested` attribute while it shows one, announced with `settings-view`:
    the window then hides the pane's header and its own back link.
  - Search rows in a view name it (`SearchRow.path`, "general?view=fonts").
- `settings-navigate` (detail: a path) asks the window to show a path.

## General and Profiles

- **General** (`<compass-settings-general-pane>`, `general_pane.ts`):
  - Browser: the default browser, with Make Default; the search engine,
    a menu; "Manage search engines and site search", a view of its own
    (below).
  - Downloads: the location, with Change… (a folder picker); "Ask where to
    save each file".
  - Appearance: font size (upstream's five sizes) and page zoom (upstream's
    levels).
  - Fonts: standard, serif, sans-serif and fixed-width (the system's font
    list), and the minimum size.
  - Languages: the preferred languages by name, which open a view of their
    own (below); "Offer to translate pages"; "Check spelling as you type".
  - System: "Use graphics acceleration when available", with Relaunch once
    changed; the system's proxy settings.
- **Manage search engines and site search** (phase 5b,
  `<compass-settings-search-engines>`, `search_engines.ts`, General's
  `?view=engines`; upstream's `searchEngines` sub-page opens it):
  - Search engines (prepopulated, and the default): Make Default, Edit,
    Remove, as allowed.
  - Site search (engines the user or sites added): a switch for whether the
    command bar offers it, Make Default, Edit, Remove; Add… opens a form
    (name, shortcut, address with %s).
  - The command bar's built-in shortcuts (@tabs…) and extensions' keywords
    aren't listed.
  - Browser side: upstream's `KeywordEditorController` decides what can be
    edited, removed, made the default or turned on or off, and saves;
    `EditSearchEngineController` validates and fixes up the address, with a
    delegate that saves through the keyword editor, as upstream's
    `SearchEnginesHandler` does.
- **Languages** (phase 5c, `<compass-settings-languages>`, `languages.ts`,
  General's `?view=languages`; upstream's `languages` sub-pages open it):
  the preferred languages in order, each moved up or down, removed (never the
  last), and with a switch for whether Compass offers to translate pages in
  it; then the languages to add, with a filter. Browser side: upstream's
  `TranslatePrefs` (`GetLanguageInfoList()`, `GetUserSelectedLanguageList()`,
  `AddToLanguageList()`, `RemoveFromLanguageList()`, `RearrangeLanguage()`,
  `BlockLanguage()`/`UnblockLanguage()`; a language added starts blocked, as
  upstream's), with `intl.accept_languages` and
  `translate_blocked_languages` observed (`Page.LanguagesChanged()`).
  Then **Translation** (docs/design/translation.md, "Packs on the
  device"): "Download translation languages" (local state
  `compass.translate.model_downloads_allowed`, the consent page
  translation asks for), the downloaded languages with their size and
  Remove, and Remove All. Browser side: `GetTranslationPacks()` reads
  upstream's installed packs and their folders' sizes;
  `RemoveTranslationPack()` and `RemoveAllTranslationPacks()` go through
  upstream's `ComponentManager`, and so Compass's installer; the consent
  and every pack's path pref are observed (`Page.LanguagesChanged()`).
  Spell check's own languages (Windows and Linux) aren't listed yet.
- Upstream's `search`, `defaultBrowser`, `downloads`, `appearance` and
  `fonts` sub-pages open General at their rows.
- **Profiles** adds `<compass-settings-profile-section>`
  (`profile_section.ts`) before Sync and Import: every profile (the
  window's own first, marked, then as the profile menu lists them), each
  with its name, saved when the field is left, and how many spaces it has
  in the switcher. "New profile" makes one (`CreateProfileForSpaces()`, in
  the sync chain when it runs), which joins the switcher with its starting
  space. Delete… asks, then schedules the profile's deletion
  (`DeleteProfileHelper`); the last profile can't go. A profile that isn't
  loaded is renamed in `ProfileAttributesStorage` only. There's no
  picture: Compass doesn't show one.
- **Spaces** (`<compass-settings-spaces-pane>`, `spaces_pane.ts`, a pane of
  its own): every profile's spaces (`SpaceDirectory`), grouped by profile,
  each group with "New space" (`AddSpaceToProfile()`, which loads the
  profile and brings it into the switcher if it wasn't). A space's row
  edits its name and moves it up or down among its profile's spaces (in
  the switcher's order); Edit… opens what the sidebar's space editor bubble has:
  the icon, the theme (`<compass-theme-editor>`, given the window's handler),
  the profile (or a new one), the archive delay, sync (turning it off
  asks), Export… (that space as a bookmarks file) and Delete (asks). A
  last row exports every space with the favorites (spaces.md, "Export").
  A viewer's shared space can't be changed.
  `SettingsWindowHandler` (`settings_window_handler_profiles.cc`) does the
  edits with the sidebar's own code (`space_mojo.{h,cc}`), on any profile's
  space, and sends `ProfilesChanged()` and `SpacesChanged()` whenever the
  directory or a profile's attributes change.
- **No chrome.send.** Upstream bans new users of `chrome.send()` and `cr.js`
  in trusted WebUI (the `legacy_chrome_send_users` list in
  `build_webui.gni`, enforced by ESLint). So upstream's handlers
  (`DefaultBrowserHandler`, `SearchEnginesHandler`, `DownloadsHandler`,
  `FontHandler`, `SystemHandler`, `ProfileInfoHandler`,
  `ManageProfileHandler`) aren't registered. `SettingsWindowHandler` does
  what they do over Mojo (`settings_window_handler_general.cc`):
  - `shell_integration::DefaultBrowserWorker` for the default browser;
  - `TemplateURLService` for the search engines it shows in its default
    list and can make the default (observed);
  - `DownloadPrefs` and a `SelectFileDialog`, setting the download and Save
    As folders;
  - `content::GetFontListAsync()`;
  - `settings_utils::ShowNetworkProxySettings()`;
  - `ProfileAttributesStorage` (observed),
    `profiles::UpdateProfileName()` and `DeleteProfileHelper`
    (`settings_window_handler_profiles.cc`).
- **Prefs** (`prefs.ts`, `SettingsWindowHandler::GetPrefs()`, `SetPref()`,
  `Page.PrefChanged()`), instead of `settingsPrivate`:
  - An allowlist (`kAllowedPrefs`) with each pref's type and whether it's
    Local State's. Values go as JSON.
  - A pref set by policy or an extension is `enforced`: its control is shown
    disabled.
  - The page asks for the prefs it shows, and the handler then follows them
    (`PrefChangeRegistrar`).
  - The default zoom has methods of its own (`ChromeZoomLevelPrefs`, as
    `settingsPrivate` does).
- **Controls** (`window.css`): switches (`input.switch`), popup menus
  (`select.popup`), pill buttons (`button.pill`, `.accent`) and fields
  (`input.field`) in rows that aren't buttons (`.control-row`).
- **Search** finds the rebuilt panes' rows by the `searchRows` `panes.ts`
  lists for them. The element marks each row with `data-row`, which search
  scrolls to and highlights.

## Privacy

- **The pane** (`<compass-settings-privacy-pane>`, `privacy_pane.ts`) has
  views, each with a ‹ back link:
  - **Main:** Clear browsing data…; third-party cookies (Allow, Block in
    Incognito, Block: `profile.cookie_controls_mode`) and Do Not Track;
    Safe Browsing (Enhanced, Standard, Off), "Always use secure connections"
    (HTTPS-Only), secure DNS (automatic or off; a custom provider set
    upstream is kept and said), Manage certificates
    (chrome://certificate-manager in a tab); the ad blocker's switch and
    Filter lists; Permissions, Sites with their own permissions, and Safety
    check (below).
  - **Ad blocker** (docs/design/ad-blocker.md): each filter list with its
    switch, rule count, last update and error; Remove for the user's lists;
    a field to add a list by address; Update Now.
  - **Clear:** the time range (upstream's `TimePeriod`s) and the data
    types: history, download history, cookies and site data, cache,
    passwords, autofill, site settings. The choices are kept in upstream's
    `browser.clear_data.*` prefs, as its dialog keeps them.
  - **Permissions:** every permission's default, with the values it can
    take, the common ones first.
  - **Sites:** the sites with permissions of their own.
  - **Site:** a site's permissions ("Default (Ask)" for one that follows the
    default), Reset Permissions and Delete Site Data (its cookies, storage
    and cache).
  - **Safety check** (phase 5e): updates (Check Now, through the updater),
    passwords (compromised, weak and reused ones the password manager knows
    of, `PasswordsPrivateDelegate::GetInsecureCredentials()`, not muted;
    Review opens the password manager's checkup; whether they leaked can't
    be checked online without a Google account, which Compass doesn't have,
    and the pane says so), Safe Browsing (Turn On when it's off), extensions
    (those the blocklist turned off; Manage opens chrome://extensions), and
    site permissions. A dot says whether each needs something.
- **The path picks the view:** `privacy?view=clear|permissions|sites|safety|adblock`,
  `privacy?site=<origin>`, `privacy?focus=security|cookies`. Upstream's
  sub-pages map to them (`SettingsWindowPathFor()`): `privacy`, `security`,
  `cookies`, `clearBrowserData`, `safetyCheck`, `content`, `content/<permission>`,
  `content/all` and `content/siteDetails?site=`. Page info's "Site settings"
  (`chrome::ShowSiteSettings()`) lands on the site: the `chrome_pages.cc`
  patch asks `MaybeShowSettingsWindow()` before it navigates a tab
  (2026-11-26; it used to open a tab for the throttle to cancel and close,
  which stayed behind, empty, when the window was already open).
- **Browser side** (`settings_window_handler_privacy.cc`), as upstream's
  `SiteSettingsHandler` and `ClearBrowsingDataHandler`:
  - The permissions are `site_settings::GetVisiblePermissionCategories()`
    that are content settings with more than one value (the registry's
    `ContentSettingsInfo` gives the values a default or a site can take).
    They're named by upstream's group names ("location",
    "media-stream-camera"), which the page labels.
  - Defaults: `HostContentSettingsMap::GetDefaultContentSetting()` and
    `SetDefaultContentSetting()`. A default from another provider than the
    user's (policy, an extension) is managed.
  - Sites: the user's single-origin exceptions
    (`site_settings::GetSingleOriginExceptionsForContentType()`) by origin.
    A site's permissions: the effective setting
    (`site_settings::GetContentSettingForOrigin()`), and whether it's the
    site's own (a user setting whose pattern isn't the wildcard).
    `SetContentSettingDefaultScope()` sets one, or resets it to the default.
  - The map is observed: `Page.ContentSettingsChanged()` reloads the
    view.
  - Clearing: `BrowsingDataRemover::RemoveAndReply()` over
    `CalculateBeginDeleteTime()` for browsing data, `RemoveWithFilterAndReply()`
    with an origin filter for a site, each with an observer that answers the
    page once done.
  - Safe Browsing: `safe_browsing::SetSafeBrowsingState()`, which keeps
    `safebrowsing.enabled` and `.enhanced` coherent. Those two prefs are read
    through the allowlist, which won't set them.
- **Not done:** the sites list shows only sites with permissions, not every
  site with stored data, as upstream's "All sites" does, nor its storage.
  Chooser permissions (the USB, serial or HID devices a site may use) and
  file system grants aren't listed.

## Passwords

- **The pane** (`<compass-settings-passwords-pane>`, `passwords_pane.ts`):
  - Passwords: "Offer to save passwords and passkeys"
    (`credentials_enable_service`), "Sign in automatically"
    (`credentials_enable_autosignin`), and the password manager
    (chrome://password-manager, in a tab).
  - Saved passwords, with a filter: each site and username, with Show
    (after the system's authentication; hidden again after 30 seconds, or
    when the pane goes), Copy and Remove. Passkeys are listed and removed,
    never shown.
  - Payment methods: "Save and fill payment methods", the cards (name, last
    four digits, expiry; Compass's own can be edited and removed, account
    ones can't), and Add Card….
  - Addresses: "Save and fill addresses", the addresses (name, street and
    city), Edit, Remove, and Add Address….
  - **The editors** (phase 5d) open under their group. A card: name on card,
    number (a new card only; the saved number never goes to the page),
    expiry month and year, nickname; the browser checks the number and that
    the date hasn't passed (`CreditCard::HasValidCardNumber()`,
    `IsValidCreditCardExpirationDate()`) and adds or updates it in the
    `PaymentsDataManager`. An address: country (named by
    `Intl.DisplayNames`), name, organization, street, city, state, postal
    code, phone, email, set as user-verified and finalized
    (`AutofillProfile::FinalizeAfterImport()`), then added or updated in the
    `AddressDataManager`.
- **Routing:** upstream's `passwords` sub-page is the pane; `payments`,
  `autofill`, `contactInfo` and `addresses` open it at their group.
- **Browser side** (`settings_window_handler_passwords.cc`):
  - Upstream's `PasswordsPrivateDelegate` (shared per profile, from
    `PasswordsPrivateDelegateFactory`), as the password manager page uses it.
    `GetSavedPasswordsList()` gives the list, with ids; the handler keeps
    each one's store, to `RemoveCredential()` it from there.
  - `RequestPlaintextPassword()` asks the system to authenticate first
    (`kView` returns the password, `kCopy` copies it in the browser and
    clears the clipboard later, so the page never has it).
  - The presenter is observed: `Page.PasswordsChanged()`.
  - Cards and addresses: the `PersonalDataManager`'s payments and address
    managers (`GetCreditCards()`, `RemoveByGUID()` for local cards only;
    `GetProfiles()`, `RemoveProfile()`), observed:
    `Page.AutofillChanged()`.
  - The prefs are on the allowlist.
- **Not done:** adding and editing passwords in the window; the checkup;
  passkeys' details; import and export. The password
  manager (chrome://password-manager) stays upstream's page.

## Advanced

- **The pane** (`<compass-settings-advanced-pane>`, `advanced_pane.ts`):
  - Performance: memory saver (`performance_tuning.high_efficiency_mode.
    state`, off or on) and energy saver (`…battery_saver_mode.state`: off,
    when the battery is low, when unplugged, always). Both are Local State's.
  - Accessibility: live caption, highlighting the focused object, caret
    browsing.
  - Reset settings, confirmed in the row: upstream's `ProfileResetter` with
    every flag and the default brandcoded settings, as chrome://settings/
    reset does.
  - Development Mode: a row with its sites, opening its page (`devMode`).
- **About** (`<compass-settings-about-pane>`, `about_pane.ts`), the last pane:
  Compass's icon (`settings_window/compass_logo.svg`, a copy of
  `assets/compass_glyph.svg`: upstream's channel logo is 32 px), name, version
  (Compass's, `compass::updates::CompassVersion()`, with the channel, and
  Chromium's), the update row (Check for Updates or Relaunch to Update), the
  update channel (`compass.update_channel`: stable, beta, canary;
  docs/design/updates.md), whether the sidebar says when an update is ready
  (`compass.update_sidebar_notice`, docs/design/sidebar.md, "Update ready"),
  and the open-source licenses. It draws its own
  heading instead of the window's pane header.
- **Routing:** upstream's `performance`, `accessibility` and `reset` are the
  Advanced pane (`advanced?focus=…`); `help` is About.
- **Browser side** (`settings_window_handler_advanced.cc`): the updater is
  `compass::updates::GetUpdater()` (Sparkle on macOS; none in builds that
  don't update), whose `CheckNow()` progress goes to the page
  (`Page.UpdateStatusChanged()`). The prefs are on the allowlist, which also
  checks values: the channel's three names, memory saver's two states,
  energy saver's four, the cookie modes, and secure DNS's off and automatic.

## Visual design

The window takes the sidebar's look rather than the system settings' one: it
is tinted with the space's color, and what sits on it is translucent white,
as the sidebar's tabs are over its tint (restyled 2026-11-22, after the first
look read as a plain copy of System Settings).

- **Tokens** (`window.css`, `:root`):
  - `--fg` is the window's own near-black (near-white in dark mode), with
    `--fg-dim`, `--fg-faint`, `--line`, `--tint` and `--press` mixed from it.
    Not the theme's on-surface color: that follows the browser theme while the
    backgrounds follow the system, and with the two disagreeing the text came
    out pale on light.
  - `--space` is the space's color, which app.ts sets (unset for a grey
    space). The window is a gradient of it over `--base`, 20 % to 9 % (16 % to
    6 % in dark mode); grey spaces get a neutral grey.
  - `--accent` is `--space`, or a fixed saturated blue for a grey space,
    with `--accent-soft`, `--accent-ring` and `--accent-text` (lighter in dark
    mode, so a deep color stays readable) derived from it.
  - `--group`, `--selected`, `--hover`: translucent white at 72 %, 86 % and
    45 % (6 %, 13 % and 6 % in dark mode), so the tint shows through.
  - 12 px group radius, 7 px control radius, 13 px system font, 12 px group
    captions, 14 px medium-weight option titles (in the re-skinned sections
    too) over 12.5 px details.
- **Toolbar:** a 32 px title row with the pane's name and search, and a row
  of icon-over-label tabs, on the window's tint with no band of its own. The
  selected tab is a white pill with a soft shadow and its icon in the accent,
  as the sidebar's selected tab. A hairline appears under the toolbar once the
  pane scrolls under it.
- **Groups:** captioned inset lists with hairline separators inset from the
  start. A row has a label and secondary text, and a trailing glyph: ↗ for a
  row that opens a tab, › (nudging on hover) for a view in the pane.
- **Controls:** 34 × 20 switches in the accent; popup buttons with the
  system's up and down chevrons; raised pill buttons (the main action in the
  accent).
- **Compass's sections, re-skinned** (window.css, under `#sections`, so its
  rules beat the sections' own class-only ones without touching their
  markup):
  - The heading becomes a group caption, hidden when the pane has only that
    section, since the title row names it.
  - The card becomes a group.
  - Their own `.row`s (the shortcuts' list) are reset from the window's rows,
    which share the class name.
  - Buttons become small accent buttons. `.secondary` and `.danger` become
    raised neutral ones.
  - Fields and selects get the window's field and popup styles and accent
    focus ring.
  - Their on/off checkboxes (Ctrl+Tab, tab previews, the AI pane's
    permissions) become switches, named by id. Checkboxes that pick items in
    a list, as the importer's, and consent checkboxes stay checkboxes. Radios
    take the accent (`accent-color`).
  - `--color-sys-primary` is the accent inside them, so their own
    primary-colored bits follow.
- **Icons:** line glyphs on a 24-unit grid at a 1.6 stroke, in the manner of
  the system's symbols.
- **Tiles** (2026-11-25, after the panes read as aseptic): a row starts with
  a 28 px rounded square in a topic's color with its glyph in white
  (`.tile[data-color]`, `TileColor` in `panes.ts`: blue, indigo, purple, pink,
  red, orange, yellow, green, teal, gray, slate). Colors go by topic, not by
  row: green for saving and filling, purple for appearance, orange for AI,
  red for resetting and clearing. The tile is always a row's first item, so a
  group's tiles line up, and the row's control (switch, radio, popup, pill)
  is at its end. With tiles, the hairline between rows starts at the text.
  Another app's own icon (a browser, in Import) takes a tile's place, drawn
  at 34 px to line up; a letter tile stands in when there's none.
- **Pane header:** each pane's main page starts with its tile at 44 px, its
  name and a line on what it's for (`Pane.color`, `Pane.blurb`), drawn by the
  shell. It's hidden on a section's page and in a pane's views.
- **Pages and views** start with "‹ <where you came from>" and the page's
  name, both 14 px in, so the chevron and the title line up with the rows'
  tiles below them.

## Not done

- The window doesn't remember its position. It opens centered toward the top
  of the last active window's display.
- No vibrancy (`NSVisualEffectView`) behind the window: the page paints an
  opaque gradient of the space's color.
- On Windows and Linux the page sits under the system's title bar, with
  its own title row too. A frameless window with minimize and close in the
  page's title row would suit them better.
