// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_COCOA_SHORTCUTS_MAC_H_
#define COMPASS_BROWSER_UI_COCOA_SHORTCUTS_MAC_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "ui/base/accelerators/accelerator.h"

#ifdef __OBJC__
@class NSEvent;
#else
class NSEvent;
#endif

struct KeyboardShortcutData;

namespace compass {

// Compass's shortcuts over upstream's, as in Arc (docs/design/shortcuts.md):
// Cmd+Shift+C copies the page's link (upstream's inspect element stays on
// Cmd+Option+C) and Cmd+S opens or closes the sidebar (upstream's Save Page
// As keeps its menu item, without a shortcut). Cmd+Shift+R reloads without
// the cache, as upstream.

// The main menu's key equivalents, by command (AcceleratorsCocoa). Also
// remembers |accelerators|, which the user's shortcuts then change.
void AdjustMenuAccelerators(std::map<int, ui::Accelerator>& accelerators);

// The shortcuts that aren't in the main menu
// (GetShortcutsNotPresentInMainMenu()).
void AdjustShortcutsNotInMenu(std::vector<KeyboardShortcutData>& shortcuts);

// The browser commands whose shortcuts chrome://compass-settings lists and
// the user can change (docs/design/shortcuts.md "Customizing").
struct CustomizableCommand {
  // Its key in kShortcutsPref (shortcut_settings.h).
  std::string_view id;
  int command;
  std::string_view section;
  std::string_view label;
};
base::span<const CustomizableCommand> GetCustomizableCommands();
const CustomizableCommand* FindCustomizableCommand(std::string_view id);

// |command|'s shortcuts as Compass ships them (main menu first), and the ones
// in force: the user's, if they changed them.
std::vector<ui::Accelerator> GetDefaultAccelerators(int command);
std::vector<ui::Accelerator> GetAccelerators(int command);

// The name of the main menu item or upstream shortcut outside the menu that
// isn't customizable and runs on |accelerator|, if one does. The page
// doesn't let a customizable shortcut take its keys.
std::optional<std::u16string> FindBuiltInShortcut(
    const ui::Accelerator& accelerator);

// Hooks in CommandForKeyEvent() (see patches/): while the settings page
// records keys, no key runs a command, so they reach the page.
bool IsRecordingShortcut();
// After the main menu found nothing for |event|: the command the user gave
// these keys, or NO_COMMAND when they're a default of a command the user
// changed; nullopt to go on with upstream's shortcuts outside the menu.
std::optional<int> CustomCommandForKeyEvent(NSEvent* event);

// Hook in GetDefaultMacAcceleratorForCommandId(), which upstream's menus
// show: nullopt when the user didn't change |command|'s shortcut, else
// whether it has one (|accelerator|).
std::optional<bool> CustomAcceleratorForCommand(int command,
                                                ui::Accelerator* accelerator);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_COCOA_SHORTCUTS_MAC_H_
