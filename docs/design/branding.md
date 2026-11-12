# Branding

Status: strings, `compass://`, the logo and the Web Store button done. Based on Chromium
157.0.8103.51.

## Goal

Nothing the user reads says Chromium where it means this browser. The
product's own name comes from `BRANDING` (window titles, the app menu,
About, the bundle). This covers the rest: upstream's strings that name the
product outright, and the `chrome://` scheme (to become `compass://`).

## Strings

Upstream keeps product-named text in Chromium-branded strings files, plus a
few messages in shared ones (in `<if expr="not _google_chrome">` branches).
Six files have any, and grit builds each from a renamed copy:

| File | Messages renamed |
|---|---|
| `chrome/app/chromium_strings.grd` (with `settings_chromium_strings.grdp`) | 676 |
| `components/components_strings.grd` (autofill, omnibox, page info, ...) | 99 |
| `components/components_chromium_strings.grd` | 20 |
| `chrome/app/generated_resources.grd` (password manager, settings) | 12 |
| `extensions/strings/extensions_strings.grd` | 5 |
| `components/privacy_sandbox_strings.grd` | 2 |

The list is `compass_rebranded_grds` in `strings/strings.gni`. To find new
ones after a Chromium update, run `rename_messages()`
(`strings/rebrand_strings.py`) over every `.grd` and see which change.

### Renaming

`strings/rebrand_strings.py` copies a `.grd`, its parts and its 81
translations to `gen/compass/strings/<same path>`, changing "Chromium" to
"Compass" in message text only (not descriptions, names or conditions):

- Whole words with ASCII boundaries: "ChromiumOS" stays, "Chromium을"
  (Korean) is renamed.
- The project keeps its name: "made possible by the Chromium open source
  project", "The Chromium Projects", and the link text between
  `BEGIN_LINK_CHROMIUM` and `END_LINK_CHROMIUM`. So About reads "Compass is
  made possible by the [Chromium] open source project".
- "The Chromium Authors" becomes "The Compass Authors" (as `BRANDING`).
- Placeholder examples (`<ex>`) are left alone.
- "Chrome" and "Google Chrome" become "Compass" too. The shared strings
  call the browser "Chrome" outright in about 480 messages that Chromium
  builds show as well: permission bubbles ("Chrome thinks you don't want
  notifications from this site"), security interstitials, page info,
  autofill dialogs, settings. Other things' names keep it
  (`_CHROME_KEEP`): the Chrome Web Store (and "Chrome Webstore"), Chrome
  Enterprise, Remote Desktop, the Chrome Root Store and Root Program,
  Chrome apps and extensions (the platforms Compass shares), Chrome OS, and
  the builds upstream names (Chrome for iOS, Chrome for Testing).

A message's translation ID is a fingerprint of its text, so renamed messages
get new IDs. The script reads both versions with grit's own parser and copies
each renamed message's translations under its new ID, with the same renaming
(translations keep "Chromium" and "Chrome" in Latin letters in almost every
language). A message that names both the product and the project, or a name
that stays ("Chrome Web Store"), keeps its translations unchanged: word
order differs, and some languages translate the rest of the name
("Интернет-магазин Chrome"), so the two can't be told apart. Each message is
judged by its own text, even when an `<if>` gives one name two messages:
"About Chrome for Testing" keeps its name, "About Chromium" (Italian
"Informazioni su Chromium") is renamed.

One message is replaced rather than renamed: the name a new profile gets
when the user gives none (`IDS_PROFILE_MENU_PLACEHOLDER_PROFILE_NAME`,
upstream "Your Chromium") is "Default", in every language (its translations
are dropped, so grit falls back to the English text). Profiles already named
"Your Chromium" or "Your Compass" by default are renamed at startup
(`compass::RenamePlaceholderProfiles()`, `space_directory.h`), since upstream
recomputes default names only once per install.

Known rough edges: Korean particles follow the old name ("Compass은" where
"Compass는" reads right), and any other language that inflects the name keeps
Chromium's form.

### Building

`patches/tools-grit-grit_rule.gni.patch`: the `grit` template, given one of
those sources, builds the copy instead and depends on
`//compass/strings:rebranded_grds`. No upstream `BUILD.gn` changes.

grit looks a `.grd`'s start IDs up by its path in
`tools/gritsettings/resource_ids.spec`. Giving the copies their own entries
there doesn't work: the spec's start IDs are computed from each file's
message count, a generated file needs a hand-written size, and extra entries
upset the spec's `join` counts (everything after `privacy_sandbox_strings`
moved by 20000 when tried). Instead the action copies each original's
computed start from upstream's `default_resource_ids` into
`gen/compass/strings/resource_ids` under the copy's path, and the template
passes that file to grit for these six. Same IDs, so `branded_strings.h`,
`generated_resources.h` and the others stay byte-identical (grit writes
headers only when they change) and nothing recompiles.

The action lists its outputs with `exec_script` at `gn gen` time, which
needs `//compass/strings/BUILD.gn` in `.gn`'s `exec_script_allowlist`
(`patches/.gn.patch`).

`InfoPlist.strings` (macOS permission prompts, "Compass Shortcut") is built
from the same paks, so it follows.

Tests: `tools/tests/test_rebrand_strings.py`.

## Windows

Windows keeps the product's identity apart from `BRANDING`: desktop and
Start menu shortcuts ("Chromium", "<profile> - Chromium"), the user data
folder (`%LOCALAPPDATA%\Chromium`), the default-browser registrations.
Two patches cover them:

- `chrome/install_static/chromium_install_modes.h` (patch): product path
  "Compass" (so `%LOCALAPPDATA%\Compass\User Data`), app name and app ID
  "Compass" (shortcuts, taskbar grouping), ProgIDs `CompassHTM` /
  `CompassPDF` with "Compass HTML/PDF Document", and Compass's own Active
  Setup GUID and toast activator, elevation service and tracing service
  CLSIDs, so it doesn't collide with a Chromium on the same PC. The
  interface IIDs stay upstream's (they come with the IDL). The
  `chromium://` direct-launch scheme stays: `compass://` is the browser's
  own scheme for its pages.
- `chrome/installer/util/BUILD.gn` (patch): the installer strings
  (`IDS_PRODUCT_NAME` is the shortcut name, the app shortcuts folder,
  firewall rule text) are generated from the renamed `chromium_strings.grd`
  copy instead of upstream's: "Compass" in every language, with the
  inflected forms noted under "Renaming".

Upstream's `install_static_unittests` check the old names and GUIDs and
need updating before they pass.

## User data folder (macOS)

An unbranded Chromium keeps its data in
`~/Library/Application Support/Chromium` (and `~/Library/Caches/Chromium`).
Compass uses `.../Compass` instead: `chrome_paths_mac.mm` (patch) asks `compass::GetMacProductDirName()` (`common/user_data_dir_mac.mm`)
when the app's Info.plist sets no `CrProductDirName`.

Earlier builds used the Chromium folder. The first launch that finds no
`Compass` folder moves the old one, if it's Compass's (it has
`Compass Spaces.json`):
a rename on the same disk, so profiles, cookies, logins and extensions come
along as they are. It then rewrites the absolute paths into the folder saved
in `Local State` and each profile's `Preferences` (the sync engine's folder,
`local_sync_backend_dir`, and others), and moves the cache folder. `Secure
Preferences` is left alone, as its values are signed; the paths there are
component extensions', which are recomputed. A plain Chromium's folder is
never touched. If a running Compass still holds the old folder (its
`SingletonLock` names a live process) or the move fails, that launch keeps
using "Chromium" and a later one moves it. The Keychain item is unaffected
(it was moved separately, `components/os_crypt/`).

Tests: `UserDataDirMacTest.*`.

## `compass://`

The user types and sees `compass://`; pages still load as `chrome://`, so
every WebUI config, `chrome://` check and content security policy upstream
(and ours: `chrome://compass-sidebar` and the others) stays as it is.
`compass://compass-sidebar` reaches the sidebar page like any other.

### In

- `compass` is a standard scheme in every process
  (`ChromeContentClient::AddAdditionalSchemes()` patch), so `compass://x/y`
  parses with a host and path.
- `compass::AddCompassURLHandler()` (`browser/compass_url_handler.cc`) is
  the first `BrowserURLHandler` pair (patch in
  `ChromeContentBrowserClient::BrowserURLHandlerCreated()`). It turns
  `compass://x` into `chrome://x` and runs the rest of the handlers on it
  itself, then returns true: the first handler that returns true ends the
  chain, so returning false would keep upstream's rewrites (new tab page,
  `chrome://about`, `chrome://help`) but leave the entry's virtual URL as
  `compass://` for good. Its reverse handler keeps the committed URL, so
  once the page commits the tab's visible URL is `chrome://x`, which is what
  upstream code checks (new tab page, WebUI, sessions). While it loads, the
  visible URL is the typed `compass://x` (`IsNewTabPage()` and the command
  bar map it back with `ToChromeURL()`).
- `ProfileIOData::IsHandledProtocol()` lists `compass` (patch): the omnibox
  and command bar take `compass://settings` as a URL, not a search; startup
  and link context menus accept it; it never goes to the OS as an external
  protocol.
- A web page can't reach `chrome://` through it. Since `compass` is a
  handled scheme, content won't let a renderer request it
  (`ChildProcessSecurityPolicy::CanRequestURL()`), so a page's
  `compass://` link or `window.open()` is blocked like one to `chrome://`.
  Test `WebPagesCantOpenCompassURLs`.

### Out

`compass/common/compass_url.h`: `ToCompassURL()` and `ToCompassText()`
write a `chrome://` URL as `compass://` where the user sees or copies it:

- the pill (`SidebarHeader::GetPillText()`), the command bar's starting text
  (Cmd+L), its entry titles and subtitles and its omnibox matches;
- Copy Link in the sidebar's context menu, and upstream's Copy URL
  (Cmd+Shift+C, `chrome::CopyURL()` patch);
- the (hidden) location bar
  (`ChromeLocationBarModelDelegate::FormattedStringWithEquivalentMeaning()`
  patch).

`chrome://` pages aren't saved in history, so nothing to do there. Saved
sidebar entries keep whatever URL the tab had (`chrome://` after commit);
both load.

### Not covered

- `view-source:compass://...` (upstream's view-source handler only takes
  the schemes it knows).
- The omnibox's built-in suggestions (`BuiltinProvider`) list `chrome://`
  pages for "chrome://" input, not for "compass://"; their text shows as
  `compass://` in the command bar.
- Upstream UI that prints a URL some other way (page info, the task
  manager, `chrome://chrome-urls` itself) still says `chrome://`.

### Rebuild cost

Six one-line hooks in `.cc` files plus `deps` in their targets; no header
anyone else includes changes, so the rebuild is small.

Tests: `compass/common/compass_url_unittest.cc`,
`SidebarHeaderTest.PillText`,
`SidebarHeaderBrowserTest.CompassSchemeOpensChromePages` and
`.WebPagesCantOpenCompassURLs`.

## Logo

Every image upstream draws Chromium's logo with is replaced by Compass's,
in place: a patch per file (binary, `git diff --binary`), so resource IDs,
grds and `BUILD.gn` files stay as they are, and anything upstream reads from
the `chromium` branding folders gets the new art, whoever reads it.

The art is two SVGs in `assets/`:

- `compass_card.svg`: the app icon's card (as `compass.svg`, without the
  shadow, edge to edge). Used where upstream's circle stands for the app.
- `compass_glyph.svg`: the two ribbons alone. Used in line with text and at
  16-32 px, where the card's white would just be a box.

`tools/branding/make_patches.py <chromium src>` renders everything
(`render.swift`, AppKit, which reads SVG), packs the `.ico`, writes the
vector icon (`make_icon.py`) and writes the patches to `patches/` without
touching the checkout, checking that each applies. Rerun it after changing
either SVG.

| Upstream file(s) | What shows it | Now |
|---|---|---|
| `chrome/app/theme/chromium/product_logo_{16,24,48,64,128,256}.png` | `IDR_PRODUCT_LOGO_*` (unscaled), app shortcut icons, notifications | card |
| `chrome/app/theme/chromium/product_logo.svg`, `product_logo_animation.svg` | `IDR_PRODUCT_LOGO_SVG`: About (settings), first run intro | card (not animated) |
| `chrome/app/theme/default_{100,200}_percent/chromium/product_logo_{16,32}.png` | `IDR_PRODUCT_LOGO_16/32`: `chrome://theme/current-channel-logo`, so the WebUI toolbar logo (settings, history, downloads, extensions), dialogs, infobars | glyph |
| `ui/webui/resources/images/chrome_logo_dark.svg` | the same toolbars in dark mode (upstream: a white Chrome logo) | glyph, in color |
| `components/vector_icons/chromium/product{,_refresh}.icon` | `vector_icons::kProductIcon` and `kProductRefreshIcon`: menus, page info for `chrome://` pages, permission and default browser prompts | glyph, one color |
| `chrome/app/vector_icons/browser_logo_old.icon` | `kBrowserLogoOldIcon`: relaunch and update toasts, the task manager, a web app window's menu | glyph, one color |
| `components/omnibox/browser/vector_icons/product_chrome_refresh_old.icon`, `product_old.icon` | `omnibox::kProductChromeRefreshOldIcon`: infobars (restore pages, default browser, PDF), app menu items, `chrome://` suggestions in the omnibox and command bar; `kProductOldIcon`: the autofill popup | glyph, one color |
| `ui/message_center/vector_icons/product_old.icon` | the header of the browser's notifications | glyph, one color |
| `chrome/app/vector_icons/chrome_product.icon`, `components/omnibox/browser/vector_icons/chrome_product.icon`, `ui/message_center/vector_icons/chrome_product.icon` | `kChromeProductIcon`: the same places with `features::kRoundedIcons` on (off by default) | glyph, one color |
| `ui/webui/resources/cr_components/searchbox/icons/chrome_product_cr23{,_old}.svg` | the WebUI searchbox's `chrome://` matches, the contextual tasks toolbar | glyph, black |
| `components/resources/default_{100,200}_percent/chromium/favicon_product.png` | favicon of built-in pages | glyph |
| `components/resources/default_{100,200}_percent/chromium/product_logo{,_white}.png` | `chrome://version` (logo and name) | glyph + "compass" |
| `chrome/app/theme/default_{100,200}_percent/chromium/product_logo_name_22{,_white}.png` | `IDR_PRODUCT_LOGO_NAME_22` | glyph + "compass" |
| `chrome/app/theme/chromium/product_logo_22_mono.png` | menu bar icon while running in the background (template image) | glyph, black |
| `chrome/browser/resources/intro/images/refresh_showcase_illustration_chromium.png` | first run intro | the card drawn over the old logo |
| `chrome/app/theme/chromium/linux/product_logo_*.png`, `default_100_percent/chromium/linux/*` | Linux window and desktop icons | card / glyph |
| `chrome/app/theme/chromium/win/chromium.ico`, `win/tiles/*.png` | Windows app icon and Start tiles | card |

The macOS app icon (`app.icns`, `Assets.car`) comes from `assets/mac/`
through `chrome-BUILD.gn.patch` (see `tools/build_mac_icon.sh`); the Dock,
notifications and the DMG use it.

Not replaced: Windows' document, PDF, incognito and app list `.ico`s, the
Linux `.xpm`, ChromeOS art, the Web Store icon (not the product's).
Upstream asks for `kCurrentProfileIconVersion` to go up when
`chromium.ico` changes; it hasn't been bumped.

## Web Store

The Chrome Web Store is a web page, so its "Add to Chrome" button isn't in
any of upstream's strings. `compass::AttachWebStoreBranding()`
(`browser/web_store/`, attached to every tab by a one-line hook in
`TabHelpers::AttachTabHelpers()`) runs a script on
`https://chromewebstore.google.com` pages at `DOMContentLoaded`, in an
isolated world (`ISOLATED_WORLD_ID_CHROME_INTERNAL`): it shares the page's
DOM, not its JavaScript, and the page can't see or undo it.

The script replaces the whole word "Chrome" with "Compass" in the text,
`aria-label` and `title` of `<button>` elements only, and a
`MutationObserver` keeps doing it as the store swaps the label (Add to,
Adding, Remove from) or renders another page in the same document. The
store's class names are generated, so the script doesn't use them. The
store keeps "Chrome" in Latin letters in every language, so "Aggiungi a
Chrome" becomes "Aggiungi a Compass". Links, headings and the store's own
name ("Chrome Web Store") stay.

Tests: `WebStoreBrandingBrowserTest.*` (a page shaped like the store's,
served as chromewebstore.google.com).
