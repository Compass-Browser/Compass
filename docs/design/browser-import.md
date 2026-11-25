# Import from another browser

Status: done for Chrome, Edge, Brave, Vivaldi, Dia, Firefox, Zen and
Safari (macOS), and for Arc's open-source successors Ardia, Cra (macOS),
Crest (macOS) and Yab (macOS); not yet run against real installs of each browser on each
platform (the readers are tested with synthetic profiles written the way
those browsers write them). Arc has its own import (arc-import.md), on the
same page.

## Goal

Move to Compass from the browsers people use without losing their place:
bookmarks, the tabs they have open, browsing history and saved passwords,
and, for browsers with spaces of their own (Zen, Vivaldi's workspaces,
Safari's tab groups, and Arc's successors: Ardia, Cra, Crest, Yab), those
spaces.

Decisions:

- **Into the profile the page is open in.** By default another browser's
  profile becomes a **space**: its bookmarks go to Pinned, its open tabs
  to Today. One space per browser profile, named "Chrome", or "Chrome –
  Work" when the browser has several profiles.
- **Or a profile of its own.** A Chromium-based browser's profile (Chrome,
  Edge, Brave, Vivaldi, Dia, Ardia, Cra) can instead become a new Compass
  profile, cookies and all, as Arc's do, with its space in it (below, "As
  a profile"). So can a Crest or Yab profile (below, "Crest's and Yab's
  profiles"). Firefox, Zen and Safari can't: their files aren't
  Chromium's.
- **Bookmarks as Pinned folders.** The bookmark tree becomes Pinned's tree;
  the bookmarks bar's entries come first, at the top level, then the
  browser's other roots ("Other bookmarks", "Bookmarks Menu"...) as folders.
- **Spaces stay spaces.** Zen's spaces, Vivaldi's workspaces and Safari's
  tab groups each become a Compass space, as Arc's do.
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
| Ardia | `%LOCALAPPDATA%\Ardia WebUI\Chromium`, or the older shell's `…\Ardia\User Data` | `…/com.ardia.browser/Chromium` | `~/.config/ardia` |
| Cra | — | `…/Cra/Chromium` | — |
| Crest | — | `…/Crest` (its own folder; `ControlPlane/session.sqlite` must be there) | — |
| Yab | — | `…/Yab`, or `…/Fast` from before October 2026 (`spaces.json` or `session.json` must be there) | — |
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
- Windows: `v10` + 12-byte nonce + AES-256-GCM (`arc::ChromiumWindowsCipher`,
  as for Arc on Windows); the key is Local State's
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
| Last session's tabs | `LastSession.plist` (`SessionWindows[].TabStates[]`: `TabURL`, `TabTitle`; `IsPrivateWindow`), in Safari's container or `~/Library/Safari`; where there's none (recent Safari), SafariTabs.db's windows (below) | `ParseSafariLastSession()`, `ReadSafariTabs()` |
| Tab groups | `SafariTabs.db` (SQLite, Safari 15 and later) in Safari's container (`~/Library/Containers/com.apple.Safari/Data/Library/Safari`), or `~/Library/Safari` | `ReadSafariTabs()` |
| History | `~/Library/Safari/History.db` (`history_items`, `history_visits`; times in seconds since 2001) | `ReadSafariHistory()` |

`ReadPlist()` turns a property list into `base::Value`s, so the parsing
runs, and is tested, on every platform. `~/Library/Safari` and Safari's
container are protected: Compass needs Full Disk Access to read them;
without it the page says so.

**SafariTabs.db** keeps tabs as bookmarks, in a `bookmarks` table
(`id`, `parent`, `type` 0 for a tab and 1 for a folder, `subtype`,
`special_id`, `hidden`, `deleted`, `order_index`, `external_uuid`, `title`,
`url`, and binary-plist `extra_attributes`/`local_attributes`), as Safari's
own queries (in its WebBookmarks framework) read it:

- Root is row 0 (`parent` NULL). Under it, with `type` 1, `special_id` 0:
  the **tab groups** of the default profile (`subtype` 0, `hidden` 0, in
  `order_index` order) and the **profiles** (`subtype` 2; the default one
  has an empty title and `external_uuid` "DefaultProfile"; its symbol is
  `SymbolImageName` in `extra_attributes`, its color a keyed-archived
  `ProfileColor` in the `settings` table). Another profile's groups are
  its children (or in a `subtype` 3 list under it).
- A group's **tabs** are its `type` 0 children (title and URL in their
  columns; a new tab has an empty URL). A pinned tab's attributes carry
  `PinnedAddress` and `PinnedTitle` (the page it was pinned at). Its
  start page's favorites are in a child folder titled
  `TopScopedBookmarkList`.
- Safari's own lists have no parent: `pinned` and `privatePinned` (the
  pinned tabs of windows outside tab groups), `recentlyClosed`, a hidden
  `Recovered` one, and each window's unnamed groups (`Local`, `Private`).
  The `windows` table names each window's (`local_tab_group_id`,
  `private_tab_group_id`, the group it shows in `active_tab_group_id`,
  `date_closed`, `is_last_session`).

`ReadSafariTabs()` reads every row not `deleted` once and walks the tree:
named groups (the default profile's, then each other profile's), and the
open windows' tabs (`pinned`'s children, then each window's `Local`
group; private ones left out). The pinned flag is found by looking for
`PinnedAddress` in the attributes' bytes, so nothing there is decoded and
the reader is tested on every platform. The layout is from Safari's
schema and queries and a Safari install's structure (no data); a filled
tab group, and its pinned tabs and favorites in particular, haven't been
checked against a real install yet.

### Arc's successors: Ardia, Cra, Crest, Yab

Open-source browsers made to replace Arc (which stops opening on 22
December 2026), each with a sidebar of spaces, pinned tabs and folders it
keeps in a file of its own. Read from their source code (Ardia 473eeff,
Cra af459f8, Crest b36496c, Yab 31df560, all November 2026); not yet
against real installs. Shared helpers: `app_sidebar.{h,cc}` (JSON, colors,
icons, ISO 8601 times, `FolderTreeBuilder`, `MergeHistory()`, the
bookmarks space).

- **Ardia** (`ardia_reader.{h,cc}`): Chromium-based (Helium), its profiles
  Chromium profiles in User Data, read as Chrome's for bookmarks, history
  and passwords (Keychain item "Ardia Safe Storage"; Windows' DPAPI key;
  Linux "v10"). The sidebar is in `session.json` beside User Data (Linux:
  `<profile>/Ardia/linux-session.json`), version 11: `profiles` (Ardia
  has no spaces apart from its profiles, as Dia), `windows` (`tabIds` in
  sidebar order), `tabs` (`pin`: null, `{"as": "tile", "folder"}` for
  Top Apps, `{"as": "row"}`; before version 5 `pinned`, `pinnedAsRow`,
  `tileFolder`; `pinnedUrl`, `customTitle`, `lastActiveAt` in ms),
  `groups` (`pinned`, `tabIds`, `parentId`), `splits` (`slots`),
  `parkedPins` (the pins of a profile no window shows). An Ardia profile
  is in the folder `Default` when its data ID (`dataId`, else its ID) is
  "default", else `Profile <data ID>`; several may share one. Pins are
  copied into every window showing their profile, linked by `pinKey`, and
  read once. Chromium's session files aren't written for Ardia's windows;
  without a session.json (an older Ardia), they're read as Chrome's.
- **Cra** (`cra_reader.{h,cc}`, macOS): Chromium-based, User Data
  `~/Library/Application Support/Cra/Chromium` (profiles `Default` and
  `Profile <UUID>`), read as Chrome's. Keychain item "Cra Safe Storage",
  used only once User Data has a file of that name; before it Cra's
  Chromium runs with the mock Keychain, whose password is
  "mock_password". The sidebar is `sidebar.json` beside User Data:
  `spaces` (`spaceOrder`; `profileID`, null for Default; `emoji`,
  `symbol`, `theme` with hex `accent`/`colors`, `intensity`, `noise`; a
  `standaloneWindowID` space is a Blank Window's and is left out) and a
  flat list of `items` (`children` give the tree and its order; `data` is
  a Swift enum: `{"container": {"_0": {"favorites"|"pinned"|"today":
  {"spaceID"}}}}`, `{"folder": {}}`, `{"tab": {"_0": {url, savedURL,
  savedTitle, homeTitle, muted, timeLastActiveAt}}}`, `{"split": …}`).
  A favorites container whose space is all zeros is shared by the
  profile's spaces.
- **Crest** (`crest_reader.{h,cc}`, macOS): Chromium for pages, one
  Chromium profile per space (`Crest/Chromium/Crest-<profile UUID>`), but
  the sidebar and history are Crest's own, in
  `Crest/ControlPlane/session.sqlite` (table `checkpoint(part, data)`:
  `core`, the session as JSON; `history.<space UUID>`, a space's history;
  copied before it's read). `spaces` with `name`, `symbol` (SF Symbol),
  `accent` (indigo, orange, teal, rose), `branding.colors`; `tabs` in
  order, each with `placement` (`pinned`: the icon grid; `saved`;
  `current`: open), `savedURL`, `folderID`, `splitGroupID`,
  `lastActivatedAt` (seconds since 2001); `folders` (`parentID`,
  `location`); IDs as `{"rawValue": "<UUID>"}`. A folder sits where its
  first tab is. A profile per Crest profile, with its spaces ("As a
  profile", below). Its passwords are in
  the data-protection Keychain under Crest's access group, which no other
  app can read (Crest exports them as CSV); its Chromium's password
  manager is off.
- **Yab** (`yab_reader.{h,cc}`, macOS): WebKit, with Chromium per tab on
  request. Everything is JSON in `~/Library/Application Support/Yab`:
  `spaces.json` (`id`, `name`, `icon`: emoji or SF Symbol, `paint` with
  hex `color`/`colors`, `grain`), a session per space (`session.json` for
  the first space, `00000000-…0001`; `session-<UUID>.json` for the
  others; `{tabs, pairs}`: a tab with `pin` is a pin, `kept` a kept tab,
  else today's; `home`/`homeTitle` the page it was kept at; `folder`,
  `folderName` and `folderPath` the folders it's in; `seen` in Unix
  seconds; `pairs` side by side), the pins every space shows
  (`session-00000000-0000-0000-0000-000000000A11.json`, read when Yab's
  defaults say `pins.everySpace`), `bookmarks.json` (a tree), and
  `history.json` / `history-<UUID>.json` (`count`, `last` in seconds
  since 2001, `typed`). Running: the app (`dev.serafim.fast`). Its
  passwords are Keychain items, each of which macOS would ask about; not
  imported.

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
| Safari tab group | A space of its own, after the "Safari" space, named after the group ("<group> – <profile>" for a profile other than the default), in a palette color no space has: its tabs in Today in Safari's order, pinned ones first; its start page's favorites in Pinned. Empty groups are left out |
| Zen space | A space of its own: name, emoji icon, theme colors (the first gradient color as the space's, up to two more as its gradient) |
| Zen pinned tabs and folders | That space's Pinned, folders nested; a pinned tab at the page it was pinned at, with the user's name for it |
| Zen split view in a folder | A split |
| Zen's other tabs | That space's Today (tab groups as folders) |
| Zen essentials | Compass's Favorites (shared by every space), those already there left out |
| Zen bookmarks | One more space, "Zen", when there are any |
| Ardia profile, Cra space, Crest space, Yab space | A space of its own: name, emoji or symbol, color (Cra's theme, Crest's branding or accent, Yab's paint, Ardia's profile color) |
| Their pinned tabs and folders (Ardia's pinned rows, Cra's Pinned, Crest's saved tabs, Yab's kept tabs) | That space's Pinned, folders nested, at the page they were pinned at, with the user's name for them |
| Their per-space favorites (Ardia's Top Apps, Cra's own favorites, Crest's pinned icons, Yab's pins) | First in that space's Pinned (a Top Apps folder as a folder) |
| Favorites shared by every space (Cra's shared favorites, Yab's "Pins in every space") | Compass's Favorites |
| Their open tabs | That space's Today, groups and folders as folders, splits (Yab's side-by-side pairs) as splits |
| Ardia's, Cra's and Yab's bookmarks | One more space, "<Browser>", when there are any |
| Crest's and Yab's history | Compass's history, one entry per page across spaces |
| History | Compass's history (`HistoryService::AddPagesWithDetails()` through `ProfileWriter`), visits marked imported |
| Passwords | Compass's password store (`ProfileWriter::AddPasswordForm()`, type "imported") |

Not imported into an existing profile: cookies (other browsers' cookies
would have to be written into Compass's live cookie store, and Chrome's
are app-bound on Windows; they come along only "As a profile", copied
before the profile loads), extensions, settings, autofill, open windows'
layout, Firefox containers, Zen's Boosts and Mods, Safari profiles other
than the default (their tab groups aside: bookmarks, history, open
windows), Safari profiles' symbols and colors, Safari's passwords
(in the Keychain), Firefox's and Zen's passwords on macOS (upstream's
Firefox importer doesn't decrypt them there).

## As a profile

For a Chromium-based browser the page offers "Add as a new profile"
beside "Add as a space here". The browser's profile folder is copied into
a new Compass profile folder before that profile loads, the way the Arc
import copies Arc's (arc-import.md, "Encryption"), so everything Chromium
keeps there comes along: cookies (you stay signed in), history, passwords,
autofill, extensions, site settings and preferences.

1. On a blocking sequence: the browser's key (`ChromiumPasswordDecryptor()`:
   macOS asks for "<Browser> Safe Storage"; Windows unwraps Local State's
   key with DPAPI; Linux, "v10" only). No key, no copy.
2. The sidebar is read from the original (bookmarks and open tabs, as
   chosen); the copy leaves the open windows out.
3. `arc::CopyProfile()` copies the folder without locks, logs, sessions
   and, unlike Arc's, the caches inside it (HTTP, code, GPU shaders:
   gigabytes after years, rebuilt as pages load). Every "v10" value of its
   databases is re-encrypted with Compass's key; cookies that don't
   decrypt are dropped, and "v20" (app-bound, Chrome on Windows) cookies
   can't come along at all. The page counts both.
4. `arc::SignCopiedPreferences()` merges Preferences and Secure
   Preferences without the browser's signatures, records the source
   profile ID in `compass.browser_import.profile`, and signs them for
   Compass (otherwise Compass would reset them, extensions included).
5. On the UI thread the profile loads (`CreateProfileAsync()`), is named
   as its space would be ("Chrome – Work"), and gets its spaces with
   `AddImportedSidebar(..., into_new_profile)`: the new profile's lone empty
   space becomes the first one. Space and item IDs are namespaced first
   (`ForNewProfile()`), since space IDs are unique across profiles and the
   same browser profile may also be imported as a space elsewhere.

**Crest's and Yab's profiles.** Both have profiles of their own, each a
set of sign-ins some of their spaces share, kept in a Chromium profile:
Crest's `Chromium/Crest-<profile UUID>` (a space's `profile.id`, or its
group's `sharedProfileID`), Yab's `Chromium/space-<UUID>` (the folder
`ChromiumProfiles/<profile UUID>-shared|named.json` records, else
`space-<profile UUID>`; a space with `ownSignIns` has its own profile, or
`signInWith`'s, the others the first space's). Each profile is one source
(`crest:<UUID>`, `yab:<UUID>`, named after its spaces: "Crest – Work,
Side"), with its spaces only; its `root` is the browser's Chromium folder,
its `path` that profile's folder (`AddAppProfiles()`, `SourceKey()`). As a
profile, the folder is copied as a Chromium-based browser's (Keychain items
"Crest Safe Storage", "Yab Safe Storage"), then the history the browser
keeps outside it (`KeepsHistoryOutsideProfile()`) is written in. A Yab
profile that never opened a site in Chromium has no folder: the new
profile starts empty, with its spaces and history. Yab's WebKit sign-ins
(cookies in WebKit's own store) and both browsers' passwords stay behind,
and the page and summary say what came (`as_profile_brings`, `brought`).
Yab's pins shown in every space and its bookmarks go with its first
profile.

**Again is harmless:** a profile whose Preferences name the source profile
marks it as imported as a profile (with the Compass profile's name), and
the copy refuses it.

**Versions:** a profile last opened by a newer Chromium than Compass's
(`Last Version` in User Data; Chrome, Edge, Brave, Vivaldi and Dia write
Chromium's version there) may have databases whose schema Compass doesn't
know: upstream razes or doesn't open those, so their data can be missing
from the copy (never from the original). The page says so; the copy still
goes ahead.

## How

- **Readers** (`browser/browser_import/`, no browser dependencies):
  `chromium_reader`, `firefox_reader`, `safari_reader` give an
  `ImportedSidebar` (spaces and favorites, in the Arc import's `arc::Space`
  and `arc::Item`), plus history entries and logins. `profile_reader`'s
  `ReadProfile()` picks the reader and reads what was chosen, on a
  blocking sequence. A Chromium-based profile's open tabs are only read
  there: `FinishReadingProfile()` parses them and builds its sidebar on
  the UI thread, because upstream's `RestoreSessionFromCommands()` draws
  session IDs from `SessionIdGenerator`, which belongs to the UI thread
  (and writes Local State).
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
  A choice with `new_profile` goes "As a profile" (above) instead.
- **Running browsers.** Crest: its Chromium's lock, or the app
  (`com.pauldavis.crest`); Yab: the app. Chromium-based: the User Data's singleton lock
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
they work on this platform). Chromium-based profiles also choose between
"Add as a space here" and "Add as a new profile"; the latter hides
History and Passwords (they come with the copy) and warns when the
browser is a newer Chromium. A running browser's profile is greyed out
with "Quit it to import", and "Check again" looks again; one imported
before shows as imported. After the import a summary says, per profile,
the new profile if one was made, the spaces made and how many tabs went
to Pinned and Today, history pages, passwords, and what didn't work
(passwords that couldn't be decrypted, cookies left behind, Safari
without Full Disk Access). "Open my spaces" shows the first new
space. Arc's section follows when Arc is there (macOS,
Windows).

**First run:** the first window opens the page if Arc's sidebar is there
and wasn't imported (arc-import.md), or if any other browser has a
profile here.

## Tests

Unit (`compass_unittests`), with synthetic profiles
(`browser/browser_import/test_fixtures.{h,cc}`: Bookmarks JSON, session
files built from upstream's session commands, History and Login Data
databases, places.sqlite, session stores and Zen's sidebar state as
mozLz4, Safari's plists as values; SafariTabs.db is written in
`safari_reader_unittest.cc`):
`ChromiumReaderTest.*`, `ChromiumCryptoTest.*`, `DetectTest.*`,
`FirefoxReaderTest.*`, `SafariReaderTest.*`, `SpaceImportTest.*`, and for
Arc's successors `AppSidebarTest.*`, `ArdiaReaderTest.*`,
`CraReaderTest.*` (Cra's own recorded fixture, trimmed),
`CrestReaderTest.*` (a session.sqlite written as Crest does) and
`YabReaderTest.*`.

Browser (`compass_browsertests`): `BrowserImporterBrowserTest.*` imports a
fixture Chrome profile (its Login Data encrypted as Chrome does on each
platform, with a DPAPI-wrapped key on Windows) and checks the space, its
Pinned and Today, the history, the passwords' count, importing again, and
the page; and imports it as a profile (a new profile named after it, its
space in place of the empty one, its source recorded, listed as imported
and refused a second time).

## Open questions

- Each browser on each platform against a real install.
- Dia on Windows: its folder is a guess.
- Ardia, Cra, Crest and Yab against real installs: their formats are read
  from their source, and they change often. Ardia on Linux: which profile
  holds `linux-session.json`, and whether release builds add a channel
  suffix to `~/.config/ardia`; its keyring name. Crest's legacy sessions
  (in its defaults, `crest.session.v2`) and archive aren't read, nor
  Cra's `archive.json`.
- Firefox's passwords on macOS: upstream's importer doesn't do them;
  reading `logins.json` with the profile's `key4.db` (NSS) would.
- Safari's tab groups against a real install with filled groups: pinned
  tabs in a group (`PinnedAddress`) and the start page's favorites
  (`TopScopedBookmarkList`) are inferred from Safari's schema, not seen.
