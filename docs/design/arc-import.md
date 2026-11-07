# Import from Arc

Status: done (reader, profile copy, sidebar, archive, the import page);
not yet run end to end on a real Arc install. macOS only. Based on Chromium
156.0.8078.48.

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
| Encryption key | Keychain generic password "Arc Safe Storage" |

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

Not imported: Arc's own features with no Compass equivalent (Boosts, Easels,
Notes, Library media), Arc's account and sync state, open windows.

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

### Page

compass://settings/importArc, a section of the settings page (settings.md),
opened by the command bar's "Import from Arc". chrome://compass-import
(`CompassArcImportUI`, `resources/sidebar/arc_import.{html,ts}`,
`sidebar.mojom` `ArcImportHandler`) shows the same section alone. It lists
Arc's profiles with their spaces, tab counts and "N archived". While Arc
runs it says so, with a "Quit Arc" button (`NSRunningApplication terminate`,
then waits up to 10 s), and won't import, since Arc's databases are open. It
explains the "Arc Safe Storage" prompt, imports the checked profiles, and
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
profile was imported from Arc (`MaybeOfferImportOnFirstRun()`, posted by
`CompassSidebarView` once the window is built). Only that first window
considers it; a later run never offers it again (the command bar's "Import
from Arc" and Settings still do).

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

`ArcSidebarTest.*`, `ChromiumMacCipherTest.*`, `ProfileCopyTest.*`,
`SidebarImportTest.*` (unit) and `ArcImporterBrowserTest.*` (browser). One
browser test builds an "Arc profile" from real Chromium data (this browser's
cookie database, re-encrypted with an Arc test key) plus Arc-signed
preferences, imports it, and checks the cookie through the network service,
the home page, and that nothing was reset.

## Open questions

- Extensions copied with the profile keep working if their IDs and files
  are there; Web Store updates then continue. To check.
- Passwords in Arc may also be in the Keychain-synced iCloud Passwords; not
  touched.
