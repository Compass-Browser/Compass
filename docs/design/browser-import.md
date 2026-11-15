# Import from another browser

Status: done for Chrome, Edge, Brave, Vivaldi, Dia, Firefox, Zen and
Safari (macOS); not yet run against real installs of each browser on each
platform (the readers are tested with synthetic profiles written the way
those browsers write them). Arc has its own import (arc-import.md), on the
same page.

## Goal

Move to Compass from the browsers people use without losing their place:
bookmarks, the tabs they have open, browsing history and saved passwords,
and, for browsers with spaces of their own (Zen, Vivaldi's workspaces),
those spaces.

Decisions:

- **Into the profile the page is open in.** Unlike Arc (whose profiles
  become Compass profiles, cookies and all), another browser's profile
  becomes a **space**: its bookmarks go to Pinned, its open tabs to Today.
  One space per browser profile, named "Chrome", or "Chrome – Work" when
  the browser has several profiles.
- **Bookmarks as Pinned folders.** The bookmark tree becomes Pinned's tree;
  the bookmarks bar's entries come first, at the top level, then the
  browser's other roots ("Other bookmarks", "Bookmarks Menu"...) as folders.
- **Spaces stay spaces.** Zen's spaces and Vivaldi's workspaces each become
  a Compass space, as Arc's do.
- **History and passwords through Chromium.** They go into Compass's
  history and password store through upstream's `ProfileWriter`, the way
  chrome://settings/importData writes them; Firefox's and Zen's go through
  upstream's Firefox importer itself.
- **Imported spaces stand out.** A space without a color of its own gets a
  palette color no space has yet.
- **Again is harmless.** Spaces get IDs derived from the browser profile's
  (as Arc's, `arc::ArcDerivedId()`); a space imported before is left as it
  is, and the page shows the profile as imported.

## Where browsers keep their profiles

Found by `DetectProfiles()` (`browser/browser_import/detect.cc`); the first
place that exists counts for each browser.

| Browser | Windows | macOS | Linux |
|---|---|---|---|
| Chrome | `%LOCALAPPDATA%\Google\Chrome\User Data` | `~/Library/Application Support/Google/Chrome` | `~/.config/google-chrome`, Flatpak `~/.var/app/com.google.Chrome/config/google-chrome` |
| Edge | `%LOCALAPPDATA%\Microsoft\Edge\User Data` | `…/Microsoft Edge` | `~/.config/microsoft-edge` |
| Brave | `%LOCALAPPDATA%\BraveSoftware\Brave-Browser\User Data` | `…/BraveSoftware/Brave-Browser` | `~/.config/BraveSoftware/Brave-Browser`, Flatpak |
| Vivaldi | `%LOCALAPPDATA%\Vivaldi\User Data` | `…/Vivaldi` | `~/.config/vivaldi` |
| Dia | `%LOCALAPPDATA%\Dia\User Data` or `…\TheBrowserCompany\Dia\User Data` (not verified) | `…/Dia/User Data` | — |
| Firefox | `%APPDATA%\Mozilla\Firefox` | `…/Firefox` | `~/.mozilla/firefox`, `~/.config/mozilla/firefox`, Snap, Flatpak |
| Zen | `%APPDATA%\zen` | `…/zen` | `~/.zen`, `~/.config/zen`, Flatpak `~/.var/app/app.zen_browser.zen/.zen` |
| Safari | — | `~/Library/Safari` | — |

Chromium-based browsers list their profiles in `Local State`
(`profile.info_cache`: folder → name; order from `profile.profiles_order`).
Firefox-based ones in `profiles.ini` (`[ProfileN]` sections with `Name`,
`Path`, `IsRelative`; the `[Install…]` section's `Default` is the profile
the browser opens, listed first). Safari has one profile Compass reads.

## Formats

### Chromium-based browsers (Chrome, Edge, Brave, Vivaldi, Dia)

`browser/browser_import/chromium_reader.{h,cc}`.

| Data | File | Read as |
|---|---|---|
| Bookmarks | `<profile>/Bookmarks` (JSON: `roots.bookmark_bar`, `other`, `synced`) | `ParseChromiumBookmarks()` |
| Open tabs | `<profile>/Sessions/Session_<time>` (SNSS command log, version 3) | `ParseChromiumSessionFile()`: upstream's `SessionCommand` and `RestoreSessionFromCommands()` |
| History | `<profile>/History` (SQLite, `urls`) | `ReadChromiumHistory()`: the newest 50,000 pages |
| Passwords | `<profile>/Login Data` (SQLite, `logins`) | `ReadChromiumLogins()` with the browser's key (below) |
| Vivaldi workspaces | `Preferences`: `vivaldi.workspaces.list` (`id`, `name`, `emoji`); a tab's workspace is in its `vivExtData` JSON (`workspaceId`) | `ParseVivaldiWorkspaces()` |

Databases are copied (with their `-wal`) to a temporary folder and read
there, so the browser's files are never written.

The session file is a header ("SNSS", version) followed by commands (a
16-bit size, an 8-bit ID, a pickled payload). Compass reads the newest
file, keeps the commands upstream knows (IDs up to 37) and replays them
with `RestoreSessionFromCommands()`: newer browsers' commands, and
Vivaldi's, would stop the replay. Vivaldi's tab data is a command upstream
doesn't have: any unknown command whose payload reads as a tab ID and a
JSON object with `workspaceId` gives that tab's workspace (and
`vivExtData` in a tab's extra data, where newer Vivaldi keeps it). Version
5 files (`Sessions_Encrypted`, encrypted with the browser's own key) aren't
read; the browsers that write them also write version 3 (Dia does).

**Passwords** (`chromium_crypto.{h,cc}`):

- macOS: `v10`, AES-128-CBC with PBKDF2-SHA1 (1003 rounds, "saltysalt")
  of the browser's Keychain item ("Chrome Safe Storage", "Microsoft Edge
  Safe Storage", "Brave Safe Storage", "Vivaldi Safe Storage", "Dia Safe
  Storage"); `arc::ChromiumMacCipher`, as for Arc. macOS asks whether
  Compass may use the item; the page says so first.
- Windows: `v10` + 12-byte nonce + AES-256-GCM; the key is Local State's
  `os_crypt.encrypted_key` ("DPAPI" + a blob `CryptUnprotectData` opens
  for the same Windows user). `v20` values (Chrome's app-bound encryption,
  Chrome 127 and later for some data) only Chrome can open: they're
  counted and left behind, and the summary says how many.
- Linux: `v10`, the same CBC scheme with the fixed password "peanuts" and
  one round. `v11` (key in the desktop's keyring: GNOME Keyring, KWallet)
  isn't read.

### Firefox and Zen

`browser/browser_import/firefox_reader.{h,cc}`.

| Data | File | Read as |
|---|---|---|
| Bookmarks | `places.sqlite`: `moz_bookmarks` (type 1 bookmark, 2 folder, 3 separator; roots `toolbar_____`, `menu________`, `unfiled_____`, `mobile______`) joined with `moz_places` | `ReadFirefoxBookmarks()` |
| Open tabs | `sessionstore.jsonlz4` (at exit), `sessionstore-backups/recovery.jsonlz4` (while running), the newest that reads | `ReadFirefoxSession()`, `ParseFirefoxSessionTabs()` |
| Zen's sidebar | `zen-sessions.jsonlz4` (`spaces`, `tabs`, `folders`, `groups`, `splitViewData`), else the session store's first window, which carries the same keys | `ReadZenSidebarState()`, `ZenSidebar()` |
| History, passwords | `places.sqlite`, `logins.json` + `key4.db` | upstream's Firefox importer (`ExternalProcessImporterHost`) |
| The app's folder | `compatibility.ini`: `LastPlatformDir` | `FirefoxAppPath()`, for upstream's NSS decryptor |

`.jsonlz4` files are "mozLz40\0", the decompressed size (32 bits,
little-endian) and one LZ4 block (`DecompressMozLz4()`, bounds-checked).

A session store tab is `{entries: [{url, title}], index (1-based),
pinned, groupId, lastAccessed (ms)}`; a window's `groups` name Firefox's
tab groups. Zen adds, per tab (Zen's `TabState.sys.mjs` patch):
`zenWorkspace` (its space's UUID), `zenEssential`, `zenSyncId`,
`zenStaticLabel` (the user's name for it), `zenIsEmpty`, `zenIsGlance`,
`_zenPinnedInitialState.entry` (a pinned tab's own page); per space
(`ZenSpaceManager`): `uuid`, `name`, `icon` (an emoji, or a chrome:// path
to one of Zen's icons), `position`, `theme.gradientColors[].c` (RGB); per
folder (`ZenFolders.storeDataForSessionStore()`): `id`, `name`,
`parentId`, `pinned`, `splitViewGroup`, `workspaceId`. Zen versions before
its session manager kept spaces and pins in places.sqlite (`zen_workspaces`,
`zen_pins`); Zen migrates them on its next start, and Compass doesn't read
them.

### Safari (macOS)

`browser/browser_import/safari_reader.{h,cc}`, `safari_reader_mac.mm`.

| Data | File | Read as |
|---|---|---|
| Bookmarks | `~/Library/Safari/Bookmarks.plist` (`WebBookmarkTypeList` with `Title` and `Children`, `WebBookmarkTypeLeaf` with `URLString` and `URIDictionary.title`; top-level lists `BookmarksBar`, `BookmarksMenu`, `com.apple.ReadingList`) | `ReadPlist()`, `ParseSafariBookmarks()` |
| Last session's tabs | `LastSession.plist` (`SessionWindows[].TabStates[]`: `TabURL`, `TabTitle`; `IsPrivateWindow`), in Safari's container or `~/Library/Safari` | `ParseSafariLastSession()` |
| History | `~/Library/Safari/History.db` (`history_items`, `history_visits`; times in seconds since 2001) | `ReadSafariHistory()` |

`ReadPlist()` turns a property list into `base::Value`s, so the parsing
runs, and is tested, on every platform. `~/Library/Safari` is protected:
Compass needs Full Disk Access to read it; without it the page says so.

### Dia

Dia (The Browser Company's browser after Arc) is Chromium-based: its
profiles are Chromium profiles in `~/Library/Application Support/Dia/User
Data` (Local State, Bookmarks, History, Login Data, Sessions), and its
Keychain item is "Dia Safe Storage". It keeps the rest of its sidebar's
state (such as its tab groups' names) in `tabs.db` and other databases
next to them, which are encrypted; Compass doesn't read them. Its
session files (version 3, alongside encrypted version 5 ones) still have
each tab with its pinned state and tab group: Dia's pinned tabs are its
sidebar pins (each in an untitled tab group), and they go to Pinned. No
public description of Dia's files exists; this is from looking at how a
Dia install lays them out (structure only).

## Mapping

| Source | Compass |
|---|---|
| A browser profile | A space, "<Browser>" or "<Browser> – <profile name>", after the user's spaces, in a palette color no space has |
| Bookmarks bar / toolbar / Safari's Favorites | Pinned, top level, first |
| Other bookmark roots (Other bookmarks, Bookmarks Menu, Mobile bookmarks, Reading List) | Pinned folders, after them (left out when empty) |
| Bookmark folders | Pinned folders, nesting kept |
| Open tabs (every normal window) | Today, in window and tab order, counting as used at the import (as Arc's, so Today's auto-archive doesn't sweep them at once) |
| A tab group with a title (Chrome, Edge, Brave, Firefox) | A Today folder |
| A tab group without one | Its tabs, loose |
| Pinned tabs (Chrome, Edge, Brave, Vivaldi, Firefox, Safari) | Today, first, as they were |
| Dia's pinned tabs | Pinned, after the bookmarks |
| Vivaldi workspace | A space of its own (name, emoji), with its tabs in Today |
| Zen space | A space of its own: name, emoji icon, theme colors (the first gradient color as the space's, up to two more as its gradient) |
| Zen pinned tabs and folders | That space's Pinned, folders nested; a pinned tab at the page it was pinned at, with the user's name for it |
| Zen split view in a folder | A split |
| Zen's other tabs | That space's Today (tab groups as folders) |
| Zen essentials | Compass's Favorites (shared by every space), those already there left out |
| Zen bookmarks | One more space, "Zen", when there are any |
| History | Compass's history (`HistoryService::AddPagesWithDetails()` through `ProfileWriter`), visits marked imported |
| Passwords | Compass's password store (`ProfileWriter::AddPasswordForm()`, type "imported") |

Not imported: cookies (other browsers' cookies would have to be written
into Compass's live cookie store, and Chrome's are app-bound on Windows;
Arc's come along only because an Arc profile becomes a new Compass
profile before it loads), extensions, settings, autofill, open windows'
layout, Firefox containers, Zen's Boosts and Mods, Safari's tab groups
(SafariTabs.db), Safari profiles other than the default, Safari's passwords
(in the Keychain), Firefox's and Zen's passwords on macOS (upstream's
Firefox importer doesn't decrypt them there).

## How

- **Readers** (`browser/browser_import/`, no browser dependencies):
  `chromium_reader`, `firefox_reader`, `safari_reader` give an
  `ImportedSidebar` (spaces and favorites, in the Arc import's `arc::Space`
  and `arc::Item`), plus history entries and logins. `profile_reader`'s
  `ReadProfile()` picks the reader and reads what was chosen.
- **Spaces** (`space_import.{h,cc}`): `AddImportedSidebar()` adds the
  spaces after the user's, with `arc::AddItems()` (the Arc import's
  builder, so folders, splits, titles and derived IDs work the same), and
  favorites not already there.
- **The import** (`browser/ui/browser_import/browser_importer.{h,cc}`):
  `GetBrowserImportOverview()` lists the profiles (with what they'd bring,
  whether their browser runs, whether they were imported);
  `ImportFromBrowsers()` reads each chosen profile on a blocking sequence,
  adds its spaces once `SpaceService` has loaded, writes its history and
  passwords through a `ProfileWriter`, or, for Firefox and Zen, runs
  upstream's `ExternalProcessImporterHost` (headless, HISTORY and
  PASSWORDS) and waits for it, then lists the profile in `SpaceDirectory`.
- **Running browsers.** Chromium-based: the User Data's singleton lock
  (`lockfile` held open on Windows, the `SingletonLock` link's process
  elsewhere). Firefox-based: `parent.lock` (Windows) or the fcntl lock on
  `.parentlock`. The page asks to quit them first (their databases are in
  use, and Firefox's importer refuses a locked profile).

## Page

"Import from Another Browser" (compass://settings/importArc, the section
the Arc import used; chrome://compass-import alone), opened from the
command bar, Settings, and on the first run (below). It lists the browser
profiles found, each with a checkbox and what to bring: "Bookmarks to
Pinned", "Open tabs to Today", "History", "Passwords" (the last two where
they work on this platform). A running browser's profile is greyed out
with "Quit it to import", and "Check again" looks again; one imported
before shows as imported. After the import a summary says, per profile,
the spaces made and how many tabs went to Pinned and Today, history pages,
passwords, and what didn't work (passwords that couldn't be decrypted,
Safari without Full Disk Access). "Open my spaces" shows the first new
space. Arc's section follows when Arc is there (macOS).

**First run:** the first window opens the page if Arc's sidebar is there
and wasn't imported (arc-import.md), or if any other browser has a
profile here.

## Tests

Unit (`compass_unittests`), with synthetic profiles
(`browser/browser_import/test_fixtures.{h,cc}`: Bookmarks JSON, session
files built from upstream's session commands, History and Login Data
databases, places.sqlite, session stores and Zen's sidebar state as
mozLz4, Safari's plists as values):
`ChromiumReaderTest.*`, `ChromiumCryptoTest.*`, `DetectTest.*`,
`FirefoxReaderTest.*`, `SafariReaderTest.*`, `SpaceImportTest.*`.

Browser (`compass_browsertests`): `BrowserImporterBrowserTest.*` imports a
fixture Chrome profile (its Login Data encrypted as Chrome does on each
platform, with a DPAPI-wrapped key on Windows) and checks the space, its
Pinned and Today, the history, the passwords' count, importing again, and
the page.

## Open questions

- Each browser on each platform against a real install.
- Dia on Windows: its folder is a guess.
- Firefox's passwords on macOS: upstream's importer doesn't do them;
  reading `logins.json` with the profile's `key4.db` (NSS) would.
- Safari's tab groups (SafariTabs.db) as spaces.
