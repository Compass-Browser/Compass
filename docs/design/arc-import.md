# Import from Arc

Status: done (reader, profile copy, sidebar, archive, Easels, the import page);
not yet run end to end on a real Arc install. macOS and Windows (below,
"Windows"). Based on Chromium 157.0.8103.51.

## Goal

Move from Arc to Compass without losing anything: every Arc profile with its
cookies (still signed in), history, cache, passwords, autofill, extensions
and settings, and Arc's sidebar: spaces (name, color, profile), pinned
entries and folders, Today tabs, and Favorites.

Decisions:

- **In the app**: an "Import from Arc" page, offered on first run and from
  the command bar / settings. Not a command-line tool.
- **Added alongside**: Arc's profiles and spaces are added next to what
  Compass has; nothing existing is touched. Importing again skips what was
  imported already.

## What Arc keeps where (macOS)

| Data | Where |
|---|---|
| Profiles (Chromium) | `~/Library/Application Support/Arc/User Data/<Default, Profile N>`; names in `Local State` → `profile.info_cache` |
| Disk cache | `~/Library/Caches/Arc/User Data/<profile>/Cache` (and `Code Cache` in the profile) |
| Sidebar | `~/Library/Application Support/Arc/StorableSidebar.json` |
| Archive | `StorableArchiveItems.json` (same folder) |
| Easels | `~/Library/Application Support/firestore/Arc/bcny-arc-server/main/` (Arc's Firestore cache, a LevelDB) |
| Encryption key | Keychain generic password "Arc Safe Storage" |

Windows: see "Windows" below. `GetArcPaths()` (`arc_import/arc_paths.h`:
`MacArcPaths()`, `FindWindowsArcPaths()`) finds them on each platform; it's
blocking, so it runs on the import's background sequence.

`StorableSidebar.json` → `sidebar.containers[1]` (the local one; `[0]` is
`{"global": …}`), which holds three Swift-encoded dictionaries, as flat
lists alternating an ID and an object:

- `spaces`: `id`, `title`, `profile` (`{"default": true}` or
  `{"custom": {"_0": {"directoryBasename": "Profile 1"}}}`), `customInfo`
  (`iconType`, `windowTheme` with its colors), `containerIDs`
  (`["pinned", <id>, "unpinned", <id>]`).
- `items`: `id`, `parentID`, `childrenIds`, `title` (a user title or null),
  `data`, one of:
  - `tab`: `savedURL`, `savedTitle`, `timeLastActiveAt`, `savedMuteStatus`
  - `list`: a folder (`title`)
  - `easel`: an Easel (`easelID`, `title`, `timeLastActiveAt`,
    `creatorID`, `shareStatus`); the board itself is in Firestore (below)
  - `itemContainer`: a space's pinned or unpinned section, or a profile's
    Favorites
  - possibly `splitView` (handled if present)
- `topAppsContainerIDs`: profile → the container of its Favorites.

## Mapping to Compass

| Arc | Compass |
|---|---|
| Profile | A new Compass profile, named as in Arc ("Arc: <name>" if Compass has that name) |
| Space | A space of that profile, listed in `SpaceDirectory` after Compass's spaces, in Arc's order |
| Space color (window theme) | A gradient theme's base colors (color, then gradient), else the mid tone, as is (`ArcSpaceColor()`, grey when unsaturated) |
| Pinned section | Pinned (folders and tabs, nesting kept) |
| Unpinned section | Today (tabs; folders become Today folders) |
| Tab's user title | Custom title (`SetCustomTitle`) |
| `timeLastActiveAt` | Last active time (Today's auto-archive counts from it) |
| Favorites (top apps) | The profile's Favorites |
| Split view | A split (two panes), else its tabs side by side |
| Archive (from a space) | The space's archive: the newest 100 (Compass's cap), the user's title else the page's, Arc's archive time |
| Archive (from Little Arc) | Not imported (no space) |
| Easel (macOS) | A board (easels.md) with its objects and images, and a tab of it where the easel was in the sidebar (below, "Easels") |

Not imported: Arc's own features with no Compass equivalent (Boosts, Notes,
Library media), Arc's account and sync state, open windows.

## How

### Reader

`compass::arc::ParseSidebar()` (`browser/arc_import/arc_sidebar.{h,cc}`)
parses `StorableSidebar.json` and `Local State` into a plain structure
(profiles, spaces, trees, favorites, colors), with no browser dependencies.
`ParseArchive()` reads `StorableArchiveItems.json` (a list alternating IDs
and entries: `sidebarItem` with `data.tab`, `archivedAt`, `source.space._0`
or `source.littleArc`, and `reason`, which isn't kept).
`ArcSidebarTest.DISABLED_RealSidebar` is an opt-in test that reads the
Arc data on the machine.

### Profile copy

`compass::arc::CopyProfile()` (`browser/arc_import/profile_copy.cc`) copies
an Arc profile folder and its disk cache into a new folder (never over an
existing one; removed again on failure), on a blocking thread, before the
profile is loaded. It leaves out lock files, logs and Arc's open windows
(`Sessions`, `Current/Last Session/Tabs`): Compass restores from its sidebar,
not Arc's windows. It then re-encrypts (below). The databases are opened
with upstream's "FirefoxImporter" metrics tag.

`compass::arc::ImportArcProfile()` (`browser/ui/arc_import/`) gets Compass's
encryptor (`os_crypt_async`), then on a sequenced background runner reads
Arc's Keychain password (`ReadArcKeychainPassword()`, `SecItemCopyMatching`
on "Arc Safe Storage"), copies the profile into
`GenerateNextProfileDirectoryPath()` and its cache into that profile's cache
folder, re-encrypts with Compass's key, and signs the preferences
(`SignImportedPreferences()`). Then it loads the profile
(`CreateProfileAsync`) and names it as in Arc.

**Preferences:** Arc signs its protected preferences (home page, startup
pages, search engine, extensions...) with its own key. macOS builds enforce
the signatures (`GROUP_ENFORCE_DEFAULT`), so Compass would reset them.
Preferences and Secure Preferences are merged without Arc's `protection`
sections and written with `chrome_prefs::InitializePrefsFromMasterPrefs()`,
the way first run writes initial preferences: signed for Compass, split into
the two files on load, where the encrypted signatures are added.

### Sidebar

`compass::arc::AddArcSidebar()` (`browser/arc_import/sidebar_import.cc`,
model only) adds a profile's Arc spaces with their names and colors, Pinned
(folders kept) and Today (folders and splits kept; a split takes two panes,
more tabs follow it). Tabs get Arc's saved title (else the host), the user's
own title as the custom title, and Arc's mute status (sidebar.md "Muted
entries"). Favorites are tabs only; folders are flattened. A new profile's
untouched "Home" space becomes the first Arc space. Pinned entries keep
Arc's last active time. Today's count as used at the import, or Today's
12 h auto-archive would take them at once. Entries come in unloaded.

Each space gets its newest 100 archived entries (`AddArchive()`), with IDs
derived from Arc's, so importing again or syncing doesn't double them. Pages
Compass doesn't restore (new tab) are left out. Arc's own pages
(`arc://downloads`, `arc://extensions`, `arc://settings`), in the archive or
the sidebar, become chrome:// ones.

`compass::arc::ImportFromArc()` reads the sidebar and Local State, imports
Arc's profiles one after another, adds each one's sidebar once its
`SpaceService` has loaded, and lists the profile in `SpaceDirectory` so its
spaces show in the switcher.

### Easels

`compass::arc::ReadArcEasels()` (`browser/arc_import/arc_easels.{h,cc}`, no
browser dependencies) reads Arc's boards from its Firestore cache. Arc keeps
only a pointer in the sidebar (`Item::Kind::kEasel`, with `easelID`); the
boards are `google.firestore.v1.Document`s in Firestore's client LevelDB.
The folder is copied to a temporary one (without `LOCK`), opened with
Chromium's LevelDB (`leveldb::RepairDB()` first if it has no `CURRENT`),
and the `remote_document` table is read: each value is a
`firestore.client.MaybeDocument`, decoded by a small protobuf wire reader
(no generated protos). A document cached under more than one key counts in
its latest version (`update_time`, or a `NoDocument`'s `read_time`); deleted
ones don't count. `easels/<id>` gives the title, background and last change;
`easels/<id>/objects/<id>` the objects (`memberships` are left out).

What Arc's objects hold, as seen in real data:

- `position.rect` is in fractions of the board's width (x, y, width and
  height alike; boards scroll down), `position.zIndex` the order. Compass
  boards get 1,000 points for Arc's width (`kArcEaselWidth`).
- `shape.type.freehand` points and `shape.type.arrow` `start`/`end` are
  fractions of the object's box (0 to 1); Compass's are board points from
  the box's top left, so they're scaled by its size.
- `text.style`: `fontFamily` `clean`, `paragraphStyle` `body` and `h2` (the
  board's title), `fillStyle` `none`. `palette`: `vibrant`. Colors are
  names (`black`, `red`, `lightGreen`, `lightBlue`, ...).
- The board's own title is a text object (`titleObjectID`); it's left out,
  as Compass shows the title itself.

Mapping: text → text (`clean` → sans, `h1` → title, `h2`/`h3` → heading,
`body` → body; other likely names in `ArcFont()`/`ArcTextStyle()`); shapes
→ rectangle, circle, arrow, freehand, with Arc's color names as CSS colors
(`black` is the board's ink, so absent), rectangles and circles in a light
color filled (Arc uses them as panels behind cards), others outlined;
image → image; link → link (its preview image); `webCard` → capture, with
`captureLayoutInfo.frameRelativeToViewport` as the region,
`webContentOffset` as the scroll and `webContentSize` as the viewport, and
`dateCaptured`. Arc palettes become `paper` (`dark` for a dark one).

The sidebar entry becomes a tab whose URL is `compass://easel/<board>`
(`AddItems()`), titled as the board, where the easel was. The board's ID is
`ArcEaselUuid()` (`ArcDerivedId("easels/<id>")`), its objects'
`ArcEaselObjectUuid()` (`"easels/<id>/objects/<id>"`), so importing on a
second Mac gives the same board, which merges when they sync.

`ImportArcEasels()` (`browser/ui/arc_import/arc_easel_import.cc`) runs
after a profile's spaces are added, once its `EaselStore` has loaded:

- A board Compass doesn't have is made (Arc's title, else the sidebar's).
  One that's there with objects is left as it is (the user may have
  changed it): importing again never doubles a board. One that's there and
  empty gets Arc's objects.
- Images (Firebase Storage download URLs, which carry their token) are
  fetched with `SimpleURLLoader`, without credentials, only from
  `https://firebasestorage.googleapis.com`, no redirects, at most 20 MB,
  four at a time; then `PrepareImage()` and `AddAsset()`. An image that
  can't be had leaves its object as a link (`WithoutImages()`): an image as
  a link to its URL, a capture as a link to its page, a link without its
  preview.
- Objects are saved together, or one by one if the store refuses one (its
  limits).

Arc downloads a board's objects when it's opened, so a board never opened
on this Mac has none in the cache (not even its title object). It comes
over empty, and the result page says "N Easels came over empty... Open them
in Arc, then import again", with an "Import Easels Again" button
(`ArcImportHandler.ImportEasels()`, `ImportArcEaselsAgain()`), which quits
Arc and fills the empty boards of every profile imported from Arc. Arc's own
edits not yet sent to its server (Firestore's mutation queue) aren't read.
Arc for Windows has no Easels.

### Page

compass://settings/importArc, a section of the settings page (settings.md),
"Import from Another Browser" (other browsers first: browser-import.md;
Arc's section after them, shown when Arc is there), opened by the command
bar's "Import from Another Browser". chrome://compass-import
(`CompassArcImportUI`, `resources/sidebar/arc_import.{html,ts}`,
`sidebar.mojom` `ArcImportHandler`) shows the same section alone. It lists
Arc's profiles with their spaces, tab counts and "N archived". While Arc
runs it says so, with a "Quit Arc" button (macOS: `NSRunningApplication
terminate`; Windows: `WM_CLOSE` to Arc's windows), then waits up to 10 s,
and won't import, since Arc's databases are open. On macOS it explains the
"Arc Safe Storage" prompt. It imports the checked profiles, and
"Open my spaces" switches the window to the first imported space.

### Encryption

Chromium on macOS encrypts with AES-128-CBC (`v10` prefix), key =
PBKDF2-HMAC-SHA1(Keychain password, "saltysalt", 1003 rounds, 16 bytes),
IV = 16 spaces. Arc's password is "Arc Safe Storage"; Compass's is its own.
Every "v10" value of `Cookies`, `Login Data`, `Login Data For Account`,
`Web Data` and `Account Web Data` is re-encrypted, in any table and column,
so new encrypted columns come along (`Cookies.cookies.encrypted_value` keeps
its 32-byte domain hash prefix in the plaintext). Cookies that don't decrypt
are deleted (a cookie lost rather than garbage); other values are left as
they were. `ChromiumMacCipher` implements Chromium's macOS scheme for Arc's
side; a one-line `crypto/subtle_passkey.h` patch lets Compass derive the
key. Reading Arc's key makes macOS ask once whether Compass may use "Arc Safe Storage"; the page says so first.

On Windows the scheme is Chromium's Windows one (below, "Windows"):
`ChromiumWindowsCipher`. `GetArcDecryptor()` picks the platform's.
Values with an app-bound key (`v20`) aren't decrypted; cookies among them
are deleted, other values left; they're counted (`CopyResult::app_bound`).
The page's summary says how many cookies and passwords couldn't be
decrypted and stayed behind (`ArcImportResult::undecryptable`: the cookies
Arc's key didn't decrypt plus the app-bound values).

### Choosing

The page lists each Arc profile with its spaces, each with a checkbox. A new
profile comes with the checked spaces among its own (unchecking the profile
leaves all of them out); with none checked, it still brings its cookies,
history, passwords, settings, extensions and favorites. The import is
`ImportFromArc(ArcImportChoice)`: the Arc profiles to copy and the Arc
space IDs to bring in (nullopt for either: everything not imported yet).

### Importing again

Each imported profile records the Arc profile folder it came from
(`compass.arc_import.profile_dir`) and the Arc spaces brought into it
(`compass.arc_import.spaces`, Arc space IDs), both profile prefs
(`compass/browser/prefs/compass_prefs.h`, registered by the
`browser_prefs.cc` patch). The page shows an imported profile as such,
with its imported spaces greyed out and Arc's newer ones checked. Importing
them doesn't copy the profile again: they're added after the Compass
profile's own spaces (`ArcImportInto::kImportedProfile`; no space reused,
favorites left as the user has them), and the page says its cookies,
history and passwords stay as first imported. A space the user deleted in
Compass stays recorded, so it isn't offered again. A profile whose folder
is gone from Compass's profile list (deleted) no longer counts as imported.
The import commits the profile's prefs before it reports, so the page,
which reads the Preferences files, sees the new state at once.

### First run

On the first run (`first_run::IsChromeFirstRun()`; upstream's first-run
experience is off, privacy.md), the first normal window opens
compass://settings/importArc if Arc's `StorableSidebar.json` exists and no
profile was imported from Arc, or another browser has profiles here
(browser-import.md) (`MaybeOfferImportOnFirstRun()`, posted by
`CompassSidebarView` once the window is built). Only that first window
considers it; a later run never offers it again (the command bar's "Import
from Another Browser" and Settings still do).

## Windows

Arc for Windows is the same app (Swift, on Chromium) shipped as an MSIX
package, `TheBrowserCompany.Arc` (package family
`TheBrowserCompany.Arc_ttt1ap7aakyb4`; the suffix is the publisher's ID,
so Compass matches the name, not the whole folder).

| Data | Where |
|---|---|
| Arc's folder | `%LOCALAPPDATA%\Packages\TheBrowserCompany.Arc_<publisher ID>\LocalCache\Local\Arc` |
| Profiles (Chromium) | `User Data\<Default, Profile N>` in it; names in `Local State` as on macOS |
| Cookies | `<profile>\Network\Cookies` (Chromium's network service folder on Windows) |
| Disk cache | In the profile (`<profile>\Cache`), copied with it |
| Sidebar, archive | `StorableSidebar.json`, `StorableArchiveItems.json` in Arc's folder |
| Encryption key | `User Data\Local State` → `os_crypt.encrypted_key` |

**Why there.** An MSIX app's writes to `%LOCALAPPDATA%` are redirected to
its package's `LocalCache\Local`; Arc's manifest (version 1.128.0.436)
declares no exceptions to that, so its `%LOCALAPPDATA%\Arc` lands in the
package folder. Any app of the same Windows user can read it.
`FindWindowsArcPaths()` lists `%LOCALAPPDATA%\Packages`, takes the folders
named `TheBrowserCompany.Arc_*` in name order, and uses the first whose Arc
folder has `User Data`.

**Format.** `StorableSidebar.json` and `StorableArchiveItems.json` are the
same Swift encoding as on macOS (Zen reads both platforms' with one parser,
below); `ParseSidebar()` and `ParseArchive()` are shared.

**Encryption.** Chromium on Windows: `Local State`'s
`os_crypt.encrypted_key` is base64 of "DPAPI" and a blob that
`CryptUnprotectData` opens for the same Windows user (no prompt), giving a
32-byte key; a value is `v10`, a 12-byte nonce, then AES-256-GCM ciphertext
and tag (`ChromiumWindowsCipher`; `browser_import::UnwrapWindowsKey()`
unwraps the key). Chrome 127's app-bound encryption (`v20`, a second key
only the browser's elevation service can open) needs that service, which
an installer registers; Arc's MSIX declares no service (no
`windows.service` extension), so Arc's values should all be `v10`. Any
`v20` value is skipped and counted, as above.

**Running and quitting.** Arc runs as `Arc.exe` from its package folder
(`WindowsApps\TheBrowserCompany.Arc_…`). `IsArcRunning()` looks for
processes named `Arc.exe` whose image path has `\TheBrowserCompany.Arc_`
(another app's `Arc.exe` doesn't count); `QuitArc()` posts `WM_CLOSE` to
their visible top-level windows (`arc_importer_win.cc`).

**Preferences.** Windows enforces protected preferences as macOS does;
`SignImportedPreferences()` is the same.

### How Zen does it

Zen (MPL-2.0; read, not copied) imports Arc on macOS and Windows through
Firefox's Chromium migrator:

- [`arc_browser_migrator.patch`](https://github.com/zen-browser/desktop/blob/5e53b8123a82799a0071cb26029a3f451f5c3d10/src/external-patches/firefox/arc_browser_migrator.patch)
  adds an `ArcProfileMigrator` (`ChromeProfileMigrator` with "Arc Safe
  Storage") and Arc's User Data to `ChromeMigrationUtils`: macOS
  `~/Library/Application Support/Arc/User Data`; Windows `LocalAppData`,
  `Packages`, a folder whose name starts with `TheBrowserCompany.Arc_`,
  `LocalCache\Local\Arc\User Data`, trying matching folders in sorted
  order (its test adds decoys: another package, and an Arc package without
  data that sorts first). The sidebar is `StorableSidebar.json` next to
  User Data on both.
- [`ZenProfileMigrators.sys.mjs`](https://github.com/zen-browser/desktop/blob/5e53b8123a82799a0071cb26029a3f451f5c3d10/src/zen/migration/ZenProfileMigrators.sys.mjs)
  and [`ZenMigrationUtils.sys.mjs`](https://github.com/zen-browser/desktop/blob/5e53b8123a82799a0071cb26029a3f451f5c3d10/src/zen/migration/ZenMigrationUtils.sys.mjs)
  turn the sidebar into Zen spaces, pins, folders and essentials (other
  Arc profiles into containers), with one parser for both platforms: the
  `global` container's items, Little Arc's (`littleBrowser`) skipped.
- Passwords on Windows go through Firefox's
  [`ChromeWindowsLoginCrypto`](https://github.com/mozilla-firefox/firefox/blob/f43afc2eee18f682d5653ab290dbf9825abb731e/browser/components/migration/ChromeWindowsLoginCrypto.sys.mjs):
  the DPAPI-unwrapped `os_crypt.encrypted_key`, `v10` + 12-byte nonce +
  AES-256-GCM; anything else is tried as plain DPAPI, `v20` has no
  handling. Firefox's migrator doesn't bring cookies.

Compass differs in what it brings (the whole profile, cookies included,
re-encrypted, as on macOS), and finds the same folder the same way.

## Arc features in Compass

Features Arc has that the import would otherwise drop:

| Arc | In Compass | Needed for |
|---|---|---|
| Space icon (`customInfo.iconType`: emoji or symbol) | Done (spaces.md "Space icons"; symbols mapped to Compass's) | Spaces looking as in Arc |
| Theme gradients (`windowTheme`) | Done: base colors, intensity, grain (spaces.md "Gradients", "Intensity and grain"); not overlayColors, wheel, translucency, vibrancy | Gradient sidebars |
| Tab mute status (`savedMuteStatus`) | Done (sidebar.md "Muted entries"); imported | Muted entries staying muted |
| Archive (`StorableArchiveItems.json`) | Yes (per space); imported (newest 100 per space) | Importing it |
| Link routing (`StorableLinkRouting.json`, Air Traffic Control) | No (little-window.md "Not done") | Links from other apps going to the right space |
| Key bindings (`StorableKeyBindings.json`) | Compass has custom shortcuts (shortcuts.md "Customizing"); Arc's aren't imported | Custom shortcuts |

## Tests

`ArcSidebarTest.*`, `ArcEaselsTest.*` (a LevelDB of crafted Firestore
documents: every kind of object, deleted and repeated documents, tables and
logs, a cache without `CURRENT`; `ArcEaselsRealTest.DISABLED_RealArcEasels`
reads this Mac's), `ArcPathsTest.*`, `WindowsArcPathsTest.*` (a fake
`%LOCALAPPDATA%` with decoy packages), `ChromiumMacCipherTest.*`,
`ChromiumWindowsCipherTest.*`, `ProfileCopyTest.*`,
`ProfileCopyWindowsTest.*` (Windows' layout and scheme, with app-bound
values), `SidebarImportTest.*` (unit; synthetic data, on every platform)
and `ArcImporterBrowserTest.*` (browser). One browser test builds an "Arc
profile" from real Chromium data (this browser's cookie database, where
this platform keeps it, re-encrypted with an Arc test key: on Windows a
test key wrapped with DPAPI into Arc's Local State) plus Arc-signed
preferences, imports it, and checks the cookie through the network service,
the home page, and that nothing was reset.

## Open questions

- Extensions copied with the profile keep working if their IDs and files
  are there; Web Store updates then continue. To check.
- Passwords in Arc may also be in the Keychain-synced iCloud Passwords; not
  touched.
- Windows: not yet checked against a signed-in Arc install (its folder,
  `v10`-only values and `Arc.exe` closing on `WM_CLOSE` come from its
  package manifest and Zen's importer, not from a real profile).
