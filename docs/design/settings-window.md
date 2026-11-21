# Settings window

Status: phases 1–4 of 5 (the window, its toolbar, Compass's own sections;
General, Profiles, Privacy and Passwords rebuilt).
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
- Typing or following `chrome://settings/` (or `compass://settings/`),
  `chrome://settings/<one of Compass's sections>` or
  `chrome://compass-settings/...` in a tab opens the window instead. A tab
  that was opened only for it closes.
- **Toolbar:** the panes as icon tabs under the title, which names the pane.
  ←/→ move between tabs, and the window opens at the pane shown last.
  - General
  - Profiles: Sync, Import from Another Browser, the profile's name and
    picture
  - Links: Links from Other Apps
  - Shortcuts
  - Boosts
  - Plugins
  - AI: Claude's settings
  - Privacy
  - Passwords
  - Advanced: Development Mode, performance, accessibility, reset, About
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
   search, and Compass's sections re-skinned. Upstream's sections are rows
   that open `chrome://settings/<section>` in a tab: General, Privacy,
   Passwords and part of Advanced.
2. **Done:** General and Profiles rebuilt (below, "General and Profiles").
   Still upstream's, as rows that open it: managing search engines and site
   search, and editing the languages list.
3. **Done:** Privacy rebuilt (below, "Privacy"). Safety check still opens
   upstream's section.
4. **Done:** Passwords (below, "Passwords"). Adding and editing passwords,
   cards and addresses, the checkup, passkeys' details and import/export
   open the password manager or upstream's sections.
5. Advanced, then no more fallback: every `chrome://settings` URL opens the
   window. The chrome://settings patches that add Compass's sections
   (menu, routes, views, BUILD.gn) go.

While upstream's settings page is still reachable, it keeps Compass's
sections (docs/design/settings.md).

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
    `links?add=`). Upstream's sub-pages go on to a tab as before.
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
    `SettingsSections::CreateHandlers()`'s, as chrome://settings has them.
    Each section binds its own `SettingsHandler` (shortcuts, links, Boosts,
    plugins, development mode), and they all stay bound.
  - `SettingsWindowHandler` (`browser/ui/webui/settings/`) gives the accent
    (the profile's active space's color, or null for grey) and pushes it as
    the space changes. It sizes the window, opens upstream's sections
    (`OpenChromeSettings()`) and chrome:// pages (`OpenUrl()`) in tabs, and
    closes the window. `ShowPath()` tells the page to show a path when the
    window is opened at one while it's open. Before the page has connected,
    the window loads the path's URL instead.
- **Front end** (`resources/sidebar/settings_window/`):
  - `app.ts`/`app.html.ts`: `<compass-settings-window>` (Lit, rendered into
    the light DOM).
  - `panes.ts`: the panes and their rows.
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

## General and Profiles

- **General** (`<compass-settings-general-pane>`, `general_pane.ts`):
  - Browser: the default browser, with Make Default; the search engine,
    a menu. "Manage search engines and site search" still opens upstream's
    section.
  - Downloads: the location, with Change… (a folder picker); "Ask where to
    save each file".
  - Appearance: font size (upstream's five sizes) and page zoom (upstream's
    levels).
  - Fonts: standard, serif, sans-serif and fixed-width (the system's font
    list), and the minimum size.
  - Languages: the preferred languages by name (editing them still opens
    upstream's section); "Offer to translate pages"; "Check spelling as you
    type".
  - System: "Use graphics acceleration when available", with Relaunch once
    changed; the system's proxy settings.
- **Profiles** adds `<compass-settings-profile-section>`
  (`profile_section.ts`) before Sync and Import: the profile's name, saved
  when the field is left, and its picture (upstream's avatars).
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
  - `ProfileAttributesStorage` (observed), `profiles::UpdateProfileName()`
    and `profiles::SetDefaultProfileAvatarIndex()`.
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
    (chrome://certificate-manager in a tab); Permissions, Sites with their
    own permissions, and Safety check (upstream's, in a tab).
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
- **The path picks the view:** `privacy?view=clear|permissions|sites`,
  `privacy?site=<origin>`, `privacy?focus=security|cookies`. Upstream's
  sub-pages map to them (`SettingsWindowPathFor()`): `privacy`, `security`,
  `cookies`, `clearBrowserData`, `content`, `content/<permission>`,
  `content/all` and `content/siteDetails?site=`. Page info's "Site settings",
  which opens a tab at the last, lands on the site (the throttle).
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
    four digits, expiry; Compass's own can be removed, account ones can't),
    and "Add or edit cards" (upstream's `payments`, in a tab).
  - Addresses: "Save and fill addresses", the addresses (name, street and
    city), Remove, and "Add or edit addresses" (upstream's `contactInfo`).
- **Routing:** upstream's `passwords` sub-page is the pane. `payments` and
  `contactInfo` stay upstream's, so the pane's add/edit rows don't come back
  to it.
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
- **Not done:** adding and editing passwords, cards and addresses in the
  window; the checkup; passkeys' details; import and export. The password
  manager (chrome://password-manager) stays upstream's page.

## Visual design

- **Tokens** (`window.css`, `:root`):
  - `--fg`, `--fg-dim`, `--line`, `--tint` and `--press` are mixed from the
    theme's on-surface color.
  - `--window` is a quiet grey background, with `--group` lists on it (white,
    or lighter in dark mode).
  - `--accent` is the space's color (a fixed saturated blue for a grey space: the theme's primary is pale in dark mode), with `--accent-soft`, `--accent-ring`
    and `--accent-text` derived from it.
  - 10 px group radius, 6 px control radius, 13 px system font, 11 px group
    captions.
- **Toolbar:** a 30 px title row with the pane's name and search, and a row
  of icon-over-label tabs. The selected tab is a soft pill in the accent. The
  toolbar is faintly tinted with the accent too.
- **Groups:** captioned inset lists with hairline separators inset from the
  start. A row has a label and secondary text, and a trailing glyph: ↗ for a
  row that opens a tab.
- **Compass's sections, re-skinned** (window.css, under `#sections`, so its
  rules beat the sections' own class-only ones without touching their
  markup):
  - The heading becomes a group caption, hidden when the pane has only that
    section, since the title row names it.
  - The card becomes a group.
  - Buttons become small accent buttons. `.secondary` and `.danger` become
    raised neutral ones.
  - Fields and selects get the window's field style and accent focus ring.
  - Checkboxes and radios take the accent (`accent-color`).
  - `--color-sys-primary` is the accent inside them, so their own
    primary-colored bits follow.
- **Icons:** line glyphs on a 24-unit grid at a 1.6 stroke, in the manner of
  the system's symbols.

## Not done

- The window doesn't remember its position. It opens centered toward the top
  of the last active window's display.
- No vibrancy (`NSVisualEffectView`) behind the toolbar: the page paints an
  opaque, accent-tinted background.
- On Windows and Linux the page sits under the system's title bar, with
  its own title row too. A frameless window with minimize and close in the
  page's title row would suit them better.
- English only, as the sections.
