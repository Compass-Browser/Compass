# Import from Arc

Status: designing (2026-10-31). Based on Chromium 156.0.8078.48; Arc on the
user's Mac is Chromium 156.0.8078.74.

## Goal

Move from Arc to Compass without losing anything: every Arc profile with its
cookies (still signed in), history, cache, passwords, autofill, extensions
and settings, and Arc's sidebar: spaces (name, color, profile), pinned
entries and folders, Today tabs, and Favorites.

Decided with the user (2026-10-31):

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
  - possibly `splitView` (not on the user's Mac; handled if present)
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
| Archive | The space's archive (later) |

Not imported: Arc's own features with no Compass equivalent (Boosts, Easels,
Notes, Library media), Arc's account and sync state, open windows.

## How

### Slices

1. **Reader** (`browser/arc_import/arc_sidebar.{h,cc}`, unit tests): parse
   `StorableSidebar.json` and `Local State` into a plain structure (profiles,
   spaces, trees, favorites, colors). No browser dependencies.
2. **Profile copy**: on a blocking thread, copy an Arc profile folder into a
   new Compass profile folder (not loaded yet, so nothing is in use), and its
   disk cache; leave out lock files, `Sessions`/`Current Session` (Compass
   restores from its sidebar, not Arc's windows), and Arc-only folders.
   Re-encrypt the encrypted columns (below). Then register the profile
   (`ProfileAttributesStorage`) and load it.
3. **Sidebar**: once an imported profile's `SpaceService` has loaded, add its
   spaces, entries and favorites through `SpaceList`/`SpaceModel`, and place
   the spaces in `SpaceDirectory`. Entries come in unloaded.
4. **Page**: `compass://import` (first run, command bar "Import from Arc"):
   lists Arc's profiles and spaces with checkboxes, asks to quit Arc if it's
   running (its databases are open), explains the Keychain prompt, shows
   progress, then switches the window to the first imported space.

### Encryption

Chromium on macOS encrypts with AES-128-CBC (`v10` prefix), key =
PBKDF2-HMAC-SHA1(Keychain password, "saltysalt", 1003 rounds, 16 bytes),
IV = 16 spaces. Arc's password is "Arc Safe Storage"; Compass's is its own.
Re-encrypted columns: `Cookies.cookies.encrypted_value` (the plaintext keeps
its 32-byte domain hash prefix), `Login Data` / `Login Data For Account`
`logins.password_value`, `Web Data` `credit_cards.card_number_encrypted`,
`local_ibans.value_encrypted`, `token_service.encrypted_token` (and any other
`*_encrypted` column found). Values that fail to decrypt are dropped (a
cookie lost rather than garbage). Reading Arc's key makes macOS ask once
whether Compass may use "Arc Safe Storage"; the page says so first.

### Importing again

Each imported profile records its Arc profile folder (a pref); each imported
space keeps Arc's space ID. A second import skips those, so only new profiles
and spaces come in.

## Arc features to add to Compass first

The user (2026-10-31): features Arc has that the import would otherwise
drop get added to Compass before the import is finished. From Arc's files:

| Arc | In Compass | Needed for |
|---|---|---|
| Space icon (`customInfo.iconType`: emoji or symbol) | Done (spaces.md "Space icons"; symbols mapped to Compass's) | Spaces looking as in Arc |
| Theme gradients (`windowTheme`) | Done: base colors, intensity, grain (spaces.md "Gradients", "Intensity and grain"); not overlayColors, wheel, translucency, vibrancy | Gradient sidebars |
| Tab mute status (`savedMuteStatus`) | Done (sidebar.md "Muted entries"); imported | Muted entries staying muted |
| Archive (`StorableArchiveItems.json`) | Yes (per space) | Importing it |
| Link routing (`StorableLinkRouting.json`, Air Traffic Control) | No (little-window.md "Not done") | Links from other apps going to the right space |
| Key bindings (`StorableKeyBindings.json`) | No (plan item 14) | Custom shortcuts |

## Progress

- Slice 1 (reader) done: `compass::arc::ParseSidebar()`, `ArcSidebarTest.*`
  (7). On the user's Arc it finds 4 spaces in 3 profiles, 61 tabs in spaces
  and 2 favorites: all 63 tabs of the file (checked with the opt-in
  `ArcSidebarTest.DISABLED_RealSidebar`).
- Slice 2 (profile copy) done: `compass::arc::CopyProfile()` copies an Arc
  profile folder and its disk cache into a new folder (never over an
  existing one; removed again on failure), without locks, logs and Arc's
  open windows (`Sessions`, `Current/Last Session/Tabs`), then re-encrypts
  every "v10" value of `Cookies`, `Login Data`, `Login Data For Account`,
  `Web Data` and `Account Web Data` (any table, any column, so new
  encrypted columns come along). Cookies that don't decrypt are deleted;
  other values are left as they were. `ChromiumMacCipher` is Chromium's
  macOS scheme for Arc's side (a one-line `crypto/subtle_passkey.h` patch
  lets Compass derive the key). The databases are opened with upstream's
  "FirefoxImporter" metrics tag. `ChromiumMacCipherTest.*`,
  `ProfileCopyTest.*` (4).
- Slice 2b (a profile, end to end) done: `compass::arc::ImportArcProfile()`
  (`browser/ui/arc_import/`) gets Compass's encryptor (`os_crypt_async`),
  then on a sequenced background runner reads Arc's Keychain password
  (`ReadArcKeychainPassword()`, `SecItemCopyMatching` on "Arc Safe
  Storage"), copies the profile into `GenerateNextProfileDirectoryPath()`
  and its cache into that profile's cache folder, re-encrypts with Compass's
  key, and signs the preferences (`SignImportedPreferences()`); then loads
  the profile (`CreateProfileAsync`) and names it as in Arc.
  - **Preferences:** Arc signs its protected preferences (home page,
    startup pages, search engine, extensions...) with its own key; macOS
    builds enforce the signatures (`GROUP_ENFORCE_DEFAULT`), so Compass would
    reset them. Preferences and Secure Preferences are merged without Arc's
    `protection` sections and written with
    `chrome_prefs::InitializePrefsFromMasterPrefs()`, as first run writes
    initial preferences: signed for Compass, split into the two files on
    load, where the encrypted signatures are added. The profile records
    `compass.arc_import.profile_dir`.
  - Tests: `ArcImporterBrowserTest.*` (2). The first builds an "Arc
    profile" from real Chromium data (this browser's cookie database,
    re-encrypted with an Arc test key) plus Arc-signed preferences, imports
    it, and checks the cookie through the network service, the home page,
    and that nothing was reset.
- Slice 3 (sidebar) done: `compass::arc::AddArcSidebar()`
  (`browser/arc_import/sidebar_import.cc`, model only) adds a profile's Arc
  spaces with their names and colors, Pinned (folders kept) and Today
  (folders and splits kept; a split takes two panes, more tabs follow it),
  tabs with Arc's saved title (else the host) and the user's own title as
  the custom title, Arc's mute status (`savedMuteStatus` "muted": the entry
  opens muted, sidebar.md "Muted entries"), and the favorites (tabs only;
  folders flattened). A new
  profile's untouched "Home" space becomes the first Arc space. Pinned
  entries keep Arc's last active time; Today's count as used at the import,
  or Today's 12 h auto-archive would take them at once. Entries come in
  unloaded. `compass::arc::ImportFromArc()` reads the sidebar and Local
  State, imports Arc's profiles one after another, adds each one's sidebar
  once its `SpaceService` has loaded, and lists the profile in
  `SpaceDirectory` so its spaces show in the switcher.
  `SidebarImportTest.*` (5), `ArcImporterBrowserTest.ImportsProfilesAndSpaces`,
  `NoSidebarNoImport`.
- Slice 4 (page) done: `chrome://compass-import` (`compass://compass-import`;
  `CompassArcImportUI`, `resources/sidebar/arc_import.{html,ts}`,
  `sidebar.mojom` `ArcImportHandler`), opened by the command bar's "Import
  from Arc". Since 2026-11-01 it's compass://settings/importArc, a section
  of the settings page (settings.md); the command bar opens that, and
  chrome://compass-import shows the same section alone. It lists Arc's profiles with their spaces and tab counts,
  checked unless imported already (a Compass profile whose preferences
  record that Arc profile); while Arc runs it says so with a "Quit Arc"
  button (`NSRunningApplication terminate`, then waits up to 10 s) and won't
  import; it explains the "Arc Safe Storage" prompt; imports the checked
  profiles (`ImportFromArc(dirs)`), names them, and "Open my spaces"
  switches the window to the first imported space. Tests:
  `ArcImporterBrowserTest.ImportPage`.
- Not done: offering it on first run; per-space choice (profiles are the
  unit); the Arc features Compass lacks (above); a run on the user's real
  Arc data.

## Open questions

- Extensions copied with the profile keep working if their IDs and files
  are there; Web Store updates then continue. To check.
- Passwords in Arc may also be in the Keychain-synced iCloud Passwords; not
  touched.
