# Shortcuts

Status: done (macOS, Linux); Windows built, not yet tried on a device.
Based on Chromium 157.0.8103.51.

## Goal

Arc's shortcuts where they differ from Chrome's. Where one is taken, Compass's
wins and upstream's command loses it. The user can change them (see
"Customizing").

| Keys | Does | Upstream had |
|---|---|---|
| Cmd+Shift+C | Copies the page's link, without tracking parameters unless that's turned off (`IDC_COPY_URL`, with upstream's "Link copied" toast; see "Clean links") | Inspect element, which keeps Cmd+Option+C |
| Cmd+S | Opens or closes the sidebar (`IDC_TOGGLE_VERTICAL_TABS_COLLAPSE`); the View menu shows it | Save Page As, which keeps its File menu item without a shortcut. The sidebar toggle was Cmd+Shift+L, now free |
| Cmd+Shift+R | Reloads without the cache (`IDC_RELOAD_BYPASSING_CACHE`) | The same: nothing to change |
| Ctrl+Option+Cmd+P | Lets clicks through the picture-in-picture window, from any app, while one is open (picture-in-picture.md "Click-through") | Nothing |
| Shift+Alt+P (Windows) | The same, as Ctrl+Option+Cmd+P | Nothing |
| Cmd+Option+N | A new little window (`kNewLittleWindowCommand`, little-window.md), from any app (a system hot key, little-window.md "From any app") | New Split Tab, which keeps its Tab menu item without a shortcut |
| Shift+Alt+N (Windows, Linux) | A new little window, as Cmd+Option+N | New Split Tab |
| Cmd+Option+R | Shows or hides Reader in the active tab (`kToggleReaderModeCommand`, reader-mode.md) | Chromium's Reading mode side panel, which Compass doesn't use |
| Shift+Alt+R (Windows, Linux) | The same, as Cmd+Option+R | Chromium's Reading mode |
| Cmd+Shift+M | Mutes the active tab, or unmutes it (`kToggleTabMuteCommand`, sidebar.md "Sound") | The profile menu (`IDC_SHOW_AVATAR_MENU`), which no longer has a shortcut |
| Cmd+Z, Cmd+Shift+Z | Outside text, and only when the page passes on the keys: undo and redo closing sidebar entries (`kUndoCloseTabCommand`, `kRedoCloseTabCommand`, sidebar.md "Undo closing"). Cmd+Shift+T undoes the same | The Edit menu's Undo and Redo, which text keeps |

Cmd+T and Cmd+L open the command bar (command-bar.md "Keyboard").

## Clean links

Copying the page's link removes the query parameters that only track where
a link was shared or clicked, as Firefox's "Copy Clean Link" does:
`compass::RemoveTrackingParams()` (`common/clean_url.{h,cc}`).

- **Where:** Copy URL (Cmd+Shift+C, the command bar's Copy URL; upstream's
  `chrome::CopyURL()`, patch) and the sidebar's Copy Link, through
  `compass::LinkToCopy()` (`browser/prefs/copy_link.{h,cc}`), which also
  writes `chrome://` as `compass://`. Android: the bar's and page menu's
  Copy URL and the sidebar's Copy Link (`CopyLinkBridge`). iOS: ios.md.
- **The setting:** "Remove tracking from copied links"
  (`compass.copy.remove_tracking`, a profile pref, on by default, doesn't
  sync), under Do Not Track on Settings, Privacy and security, Third-party
  cookies (`cookies_page.html` patch; allowlisted in `prefs_util.cc`).
  Android: Settings, Privacy and Security, "Remove Tracking from Copied
  Links". Off, links are copied as they are.
- **The lists** are Firefox's strip-on-share lists, unmodified, in
  `third_party/strip_on_share/` (MPL-2.0 and LGPL-3.0 files, each with its
  license; the README names the Firefox commit and how to update). They're
  built in as strings (`:strip_on_share_lists`, `build/embed_text.py`) and
  parsed on first use. `tools/update_strip_on_share.py` downloads the
  latest; the `strip-on-share` workflow runs it every Monday and opens a
  pull request when they changed, so a release ships what's merged.
- **Their licenses** are shown in full: on the desktop, the About page's
  "third-party files under their own licenses" opens
  chrome://compass-settings/licenses.html, built from the LICENSE files by
  `build/licenses_page.py` (`:licenses_page` in `resources/sidebar/`;
  about_page.html.ts patch); on Android, Settings → Third-Party Licenses
  (the files as assets, `:license_assets`); on iOS, ios.md.
- **Matching**, as Firefox: a parameter's name, as written and without case,
  against the global rule, the rule for the exact host (`hosts`) and the
  rule for the registrable domain (`schemelessSites`). A parameter whose
  value is an http(s) URL has that URL cleaned too, one level deep.
- **Kept:** every other parameter in its order and spelling, the fragment,
  and URLs other than http and https. A query left empty loses its "?".
- Tests: `CleanUrlTest`, `CopyLinkTest`,
  `CompassShortcutsBrowserTest.CopyLinkLeavesOutTrackingParams`,
  `CopyLinkSettingsBrowserTest` (the licenses page, About's link, the
  toggle).

## How

On macOS, a shortcut is either a main menu item's key equivalent, which
comes from `AcceleratorsCocoa` for command items, or one of
`GetShortcutsNotPresentInMainMenu()` (`global_keyboard_shortcuts_mac.mm`).
Both call into `compass/browser/ui/cocoa/shortcuts_mac.h` once they're built:
`AdjustMenuAccelerators()` and `AdjustShortcutsNotInMenu()`. There's no menu
item for Copy URL, so Cmd+Shift+C is in the second list, in place of inspect
element; Cmd+Shift+M, there too, takes the profile menu's entry.

Tests: `CompassShortcutsBrowserTest` resolves key events through
`CommandForKeyEvent()`, as typing does, and runs the commands.

On Windows and Linux the same shortcuts come with Ctrl for Cmd (see
"Windows and Linux" below):
Ctrl+Shift+C copies the link (inspect element keeps F12's developer tools
and Ctrl+Shift+I), Ctrl+S opens or closes the sidebar (Save Page As keeps
its menu item), Ctrl+Shift+M mutes the tab (the profile menu loses it),
Ctrl+Shift+E chats with Claude, Shift+Alt+R shows Reader. Undo and redo of
closing tabs have no keys there: Ctrl+Z would come before the page.

## Customizing

compass://settings/shortcuts ("Keyboard shortcuts" in the settings menu or
the command bar; also alone on chrome://compass-settings, see settings.md)
lists the shortcuts the user can change, by
section: Compass's (command bar, address, sidebar, copy link, little window,
picture-in-picture click-through but on Linux, Claude, Reader), tabs, windows,
browser pages, developer tools, and the nine space shortcuts, the same on
every platform. Clicking a shortcut's keys records the next keys pressed
with ⌘, ⌃ or ⌥ (Ctrl or Alt on Windows and Linux; Esc cancels, Delete leaves it without a
shortcut). A changed shortcut shows its default and a Reset button; "Reset
all" undoes every change. The page is meant to grow into Compass's settings
(link routing for little windows is next, little-window.md).

Left out on purpose: Close Tab and Close Window (AppController moves Cmd+W
between them as windows change), Quit, Hide, copy and paste, and anything
upstream's menus handle themselves. Keys one of those has are refused
("Already used by Quit Compass").

### Keys another shortcut has

Keys can belong to one shortcut. Keys a listed shortcut has bring a
question, "⇧⌘K is the shortcut for Reload. Use it here instead?"; yes takes
them, and the other shortcut keeps its next keys if it had several (Next tab
has four) or none. Resetting a shortcut takes its defaults back from whoever
was given them since.

A changed shortcut has one set of keys (or none): Next tab's four defaults
become the one the user picked.

### Storage

The Local State pref `compass.shortcuts` (ARCHITECTURE.md D19), browser-wide
like the main menu: `{"<id>": {"key": <ui::KeyboardCode>, "modifiers":
<ui::EF_* flags>}}`, `{}` for none; shortcuts left out keep their defaults.
`compass::ShortcutSettings` (`browser/shortcuts/`) reads it once Local State
is there, writes it on every change and tells observers. It's the pref's only
writer, so it doesn't watch it (and keeps no pointer to Local State, which is
destroyed first at shutdown). Other open settings pages catch up when they
get the focus. The pref is registered by a hook in `RegisterLocalState()`
(patch).

### How the browser follows it (macOS)

- **Main menu commands** (Reload, New Window, ...): `AdjustMenuAccelerators()`
  keeps a pointer to AcceleratorsCocoa's map. Once the main menu is built (a
  task posted from there) and on every change, `ShortcutApplier` writes the
  user's keys (or the defaults it copied before its first change) into the map
  and into the menu items with that command's tag. Menus show them, and
  `CommandForKeyEvent()` finds them through the menu as before.
- **Commands outside the menu** (Copy URL, Next tab, little window, mute tab): a hook in
  `CommandForKeyEvent()`, after the menu, matches the user's keys
  (`CustomCommandForKeyEvent()`); upstream's keys for a command the user
  changed then run nothing. `GetDefaultMacAcceleratorForCommandId()`, which
  upstream's views menus show, asks `CustomAcceleratorForCommand()` first.
- **Undo and redo closing** (Cmd+Z, Cmd+Shift+Z): not in the main menu's
  map or `CustomCommandForKeyEvent()`, which run before the page. A hook in
  `DelayedWebContentsCommandForKeyEvent()` (`DelayedCommandForKeyEvent()`)
  matches their keys, the user's or the defaults, once the page passed on
  them. `FindBuiltInShortcut()` skips the Edit menu's Undo and Redo, whose
  keys they share.
- **Spaces** (Ctrl+1–9): `SpaceShortcuts` registers each space's keys with the
  window's focus manager and registers again when they change.
- **Click-through** (Ctrl+Option+Cmd+P): `PipWindows` listens for the user's
  keys and moves its global hot key when they change.
- **Recording**: while the page records, `ShortcutSettings::recording()` is
  true. `CommandForKeyEvent()` then finds nothing, so even reserved keys
  (Cmd+T, Cmd+W, Cmd+N) reach the page, which takes them
  (`preventDefault()`), and `PipWindows` drops its global hot key. Recording
  ends on Esc, on keys, when the page loses focus and when it goes away.

### How the browser follows it (Windows and Linux)

Both are views and upstream's accelerator table, so they share the code.
`compass/browser/shortcuts/shortcuts_table.{h,cc}` has the same API as
`shortcuts_mac.h` for the page (`GetCustomizableCommands()`: macOS's list
less undo and redo of closing tabs; `GetDefaultAccelerators()`,
`GetAccelerators()`, `FindBuiltInShortcut()`).

- **The table.** A window's shortcuts are upstream's accelerator table
  (`GetAcceleratorList()`), which `BrowserView::LoadAccelerators()`
  registers with the window's focus manager, and which its menus show. A
  hook at the table's end (patch) hands it to `AdjustAcceleratorList()`:
  Compass's keys (`kCompassDefaults`) go over upstream's, then the user's
  over those. The first call keeps the result before the user's as the
  defaults the page shows.
- **Changes and recording.** `BrowserViewAccelerators` (`browser/ui/views/
  frame/browser_view_accelerators.{h,cc}`, a `BrowserView` friend, patch)
  follows `ShortcutSettings`, from the moment a settings page opens:
  on a change every window unregisters its accelerators, clears its table
  and loads it again; while the page records keys, the windows register
  none, so every key (Ctrl+T, Ctrl+W) reaches the page.
- **Keys taken by upstream commands that aren't listed** (Ctrl+W, ...) are
  refused ("Already used by another browser shortcut").
- **Spaces** work as on macOS (Ctrl+Shift+1–9). **Click-through** too on
  Windows, where it can't have the Windows key (picture-in-picture.md
  "Click-through"); Linux has no picture-in-picture click-through, so its
  page doesn't list it.
- **A little window from any app** follows the shortcut as on macOS, as an
  X11 key grab on Linux (little-window.md "From any app").
- Key names: upstream's text leaves most function keys blank there and
  writes the numpad's like the main keys; the page names them itself
  (`KeyName()`, "F13", "Num 5").

Tests: `CompassShortcutsBrowserTest.CustomShortcuts`, `RecordingHoldsKeysBack`,
`SpaceShortcuts`, `SettingsPage` (records keys by key events on the page, takes
keys from another shortcut, refuses Quit's; Close Tab's on Windows and
Linux), `ShortcutSettingsTest` (unit); on Windows and Linux also
`ArcShortcuts`, `MuteTabShortcut` and `ListsBrowserCommands`
(`browser_view_accelerators_browsertest.cc`) and `ShortcutsTableTest`
(unit: Compass's keys over upstream's table, then the user's).

Not done: extension shortcuts (chrome://extensions/shortcuts has them), a
search over the list, and other platforms' browser commands.
