# Shortcuts

Status: done (macOS). Based on Chromium 156.0.8078.48.

## Goal

Arc's shortcuts where they differ from Chrome's. Where one is taken, Compass's
wins and upstream's command loses it.

| Keys | Does | Upstream had |
|---|---|---|
| Cmd+Shift+C | Copies the page's link (`IDC_COPY_URL`, with upstream's "Link copied" toast) | Inspect element, which keeps Cmd+Option+C |
| Cmd+S | Opens or closes the sidebar (`IDC_TOGGLE_VERTICAL_TABS_COLLAPSE`); the View menu shows it | Save Page As, which keeps its File menu item without a shortcut. The sidebar toggle was Cmd+Shift+L, now free |
| Cmd+Shift+R | Reloads without the cache (`IDC_RELOAD_BYPASSING_CACHE`) | The same: nothing to change |
| Ctrl+Option+Cmd+P | Lets clicks through the picture-in-picture window, from any app, while one is open (picture-in-picture.md "Click-through") | Nothing |
| Cmd+Option+N | A new little window (`kNewLittleWindowCommand`, little-window.md) | New Split Tab, which keeps its Tab menu item without a shortcut |

Cmd+T and Cmd+L open the command bar (command-bar.md "Keyboard").

## How

On macOS, a shortcut is either a main menu item's key equivalent, which
comes from `AcceleratorsCocoa` for command items, or one of
`GetShortcutsNotPresentInMainMenu()` (`global_keyboard_shortcuts_mac.mm`).
Both call into `compass/browser/ui/cocoa/shortcuts_mac.h` once they're built:
`AdjustMenuAccelerators()` and `AdjustShortcutsNotInMenu()`. There's no menu
item for Copy URL, so Cmd+Shift+C is in the second list, in place of inspect
element.

Tests: `CompassShortcutsBrowserTest` resolves key events through
`CommandForKeyEvent()`, as typing does, and runs the commands.

Other platforms' shortcuts (`accelerator_table.cc`) are unchanged.
