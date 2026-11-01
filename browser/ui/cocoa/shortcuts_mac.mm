// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/cocoa/shortcuts_mac.h"

#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/global_keyboard_shortcuts_mac.h"
#include "compass/browser/shortcuts/shortcut_settings.h"
#include "compass/browser/ui/compass_command_ids.h"
#import "ui/base/accelerators/platform_accelerator_cocoa.h"
#import "ui/base/cocoa/nsmenuitem_additions.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_code_conversion_mac.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace compass {

namespace {

// In the order the settings page shows them, by section.
constexpr CustomizableCommand kCustomizableCommands[] = {
    {"new-tab", IDC_NEW_TAB, "Compass", "New tab (command bar)"},
    {"open-location", IDC_FOCUS_LOCATION, "Compass",
     "Edit the page's address"},
    {"toggle-sidebar", IDC_TOGGLE_VERTICAL_TABS_COLLAPSE, "Compass",
     "Show or hide the sidebar"},
    {"copy-url", IDC_COPY_URL, "Compass", "Copy the page's link"},
    {"little-window", kNewLittleWindowCommand, "Compass",
     "New little window"},

    {"restore-tab", IDC_RESTORE_TAB, "Tabs", "Reopen closed tab"},
    {"next-tab", IDC_SELECT_NEXT_TAB, "Tabs", "Next tab"},
    {"previous-tab", IDC_SELECT_PREVIOUS_TAB, "Tabs", "Previous tab"},
    {"mute-tab", kToggleTabMuteCommand, "Tabs", "Mute or unmute the tab"},
    {"reload", IDC_RELOAD, "Tabs", "Reload"},
    {"hard-reload", IDC_RELOAD_BYPASSING_CACHE, "Tabs",
     "Reload without the cache"},
    {"back", IDC_BACK, "Tabs", "Back"},
    {"forward", IDC_FORWARD, "Tabs", "Forward"},
    {"find", IDC_FIND, "Tabs", "Find on page"},
    {"zoom-in", IDC_ZOOM_PLUS, "Tabs", "Zoom in"},
    {"zoom-out", IDC_ZOOM_MINUS, "Tabs", "Zoom out"},
    {"zoom-reset", IDC_ZOOM_NORMAL, "Tabs", "Actual size"},
    {"print", IDC_PRINT, "Tabs", "Print"},
    {"save-page", IDC_SAVE_PAGE, "Tabs", "Save page as"},

    {"new-window", IDC_NEW_WINDOW, "Windows", "New window"},
    {"new-incognito-window", IDC_NEW_INCOGNITO_WINDOW, "Windows",
     "New incognito window"},
    {"fullscreen", IDC_FULLSCREEN, "Windows", "Enter or exit full screen"},

    {"history", IDC_SHOW_HISTORY, "Browser", "History"},
    {"downloads", IDC_SHOW_DOWNLOADS, "Browser", "Downloads"},
    {"bookmark-manager", IDC_SHOW_BOOKMARK_MANAGER, "Browser",
     "Bookmark manager"},
    {"extensions", IDC_MANAGE_EXTENSIONS, "Browser", "Extensions"},
    {"settings", IDC_OPTIONS, "Browser", "Settings"},
    {"clear-browsing-data", IDC_CLEAR_BROWSING_DATA, "Browser",
     "Clear browsing data"},

    {"devtools", IDC_DEV_TOOLS, "Developer", "Developer tools"},
    {"console", IDC_DEV_TOOLS_CONSOLE, "Developer", "JavaScript console"},
    {"inspect", IDC_DEV_TOOLS_INSPECT, "Developer", "Inspect elements"},
    {"view-source", IDC_VIEW_SOURCE, "Developer", "View source"},
};

// AcceleratorsCocoa's map, which the main menu's items and upstream's menus
// read, and its entries as Compass ships them (taken before the user's
// shortcuts first change it, after upstream's RTL swap).
std::map<int, ui::Accelerator>* g_menu_accelerators = nullptr;

std::map<int, ui::Accelerator>& DefaultMenuAccelerators() {
  static base::NoDestructor<std::map<int, ui::Accelerator>> defaults;
  static bool taken = false;
  if (!taken && g_menu_accelerators) {
    *defaults = *g_menu_accelerators;
    taken = true;
  }
  return *defaults;
}

ui::Accelerator AcceleratorFromShortcut(const KeyboardShortcutData& shortcut) {
  int modifiers = 0;
  if (shortcut.command_key) {
    modifiers |= ui::EF_COMMAND_DOWN;
  }
  if (shortcut.shift_key) {
    modifiers |= ui::EF_SHIFT_DOWN;
  }
  if (shortcut.cntrl_key) {
    modifiers |= ui::EF_CONTROL_DOWN;
  }
  if (shortcut.opt_key) {
    modifiers |= ui::EF_ALT_DOWN;
  }
  return ui::Accelerator(ui::KeyboardCodeFromKeyCode(shortcut.vkey_code),
                         modifiers);
}

NSMenuItem* MakeKeyItem(const ui::Accelerator& accelerator, int command) {
  KeyEquivalentAndModifierMask* equivalent =
      ui::GetKeyEquivalentAndModifierMaskFromAccelerator(accelerator);
  NSMenuItem* item =
      [[NSMenuItem alloc] initWithTitle:@""
                                 action:nullptr
                          keyEquivalent:equivalent.keyEquivalent];
  item.keyEquivalentModifierMask = equivalent.modifierMask;
  item.tag = command;
  return item;
}

bool IsCustomizable(int command) {
  return std::ranges::any_of(kCustomizableCommands,
                             [command](const CustomizableCommand& entry) {
                               return entry.command == command;
                             });
}

std::optional<ui::Accelerator> CustomAcceleratorFor(int command) {
  for (const CustomizableCommand& entry : kCustomizableCommands) {
    if (entry.command == command) {
      return ShortcutSettings::Get().GetCustom(entry.id);
    }
  }
  return std::nullopt;
}

// Calls |visit| with every item of |menu| and its submenus.
template <typename Visit>
void ForEachMenuItem(NSMenu* menu, const Visit& visit) {
  for (NSMenuItem* item in menu.itemArray) {
    if (item.submenu) {
      ForEachMenuItem(item.submenu, visit);
    } else {
      visit(item);
    }
  }
}

// The main menu's items that run |command|.
std::vector<NSMenuItem*> MenuItemsFor(int command) {
  std::vector<NSMenuItem*> items;
  ForEachMenuItem(NSApp.mainMenu, [&](NSMenuItem* item) {
    if (item.tag == command && item.action) {
      items.push_back(item);
    }
  });
  return items;
}

// Upstream's shortcuts outside the menu, as menu items to match events
// against (as GetMenuItemsNotPresentInMainMenu()).
const std::vector<NSMenuItem*>& UpstreamItemsNotInMenu() {
  static base::NoDestructor<std::vector<NSMenuItem*>> items([] {
    std::vector<NSMenuItem*> items;
    for (const KeyboardShortcutData& shortcut :
         GetShortcutsNotPresentInMainMenu()) {
      items.push_back(MakeKeyItem(AcceleratorFromShortcut(shortcut),
                                  shortcut.chrome_command));
    }
    return items;
  }());
  return *items;
}

// Applies the user's shortcuts as they change: to AcceleratorsCocoa's map
// and the main menu's items for commands in the menu, and to |custom_items_|
// for the others.
class ShortcutApplier : public ShortcutSettings::Observer {
 public:
  static ShortcutApplier& Get() {
    static base::NoDestructor<ShortcutApplier> instance;
    return *instance;
  }

  ShortcutApplier() { ShortcutSettings::Get().AddObserver(this); }

  void Apply() {
    // Not while AcceleratorsCocoa builds its map: the posted Apply() comes
    // once the menu is there.
    if (!g_menu_accelerators || !NSApp.mainMenu) {
      return;
    }
    const std::map<int, ui::Accelerator>& defaults = DefaultMenuAccelerators();
    custom_items_.clear();
    for (const CustomizableCommand& entry : kCustomizableCommands) {
      const std::optional<ui::Accelerator> custom =
          ShortcutSettings::Get().GetCustom(entry.id);
      std::vector<NSMenuItem*> menu_items = MenuItemsFor(entry.command);
      if (menu_items.empty()) {
        // Outside the menu: CustomCommandForKeyEvent() matches it.
        if (custom && custom->key_code() != ui::VKEY_UNKNOWN) {
          custom_items_.push_back(MakeKeyItem(*custom, entry.command));
        }
        continue;
      }
      std::optional<ui::Accelerator> accelerator;
      if (custom) {
        if (custom->key_code() != ui::VKEY_UNKNOWN) {
          accelerator = *custom;
        }
      } else if (auto it = defaults.find(entry.command); it != defaults.end()) {
        accelerator = it->second;
      }
      if (accelerator) {
        (*g_menu_accelerators)[entry.command] = *accelerator;
      } else {
        g_menu_accelerators->erase(entry.command);
      }
      KeyEquivalentAndModifierMask* equivalent =
          accelerator
              ? ui::GetKeyEquivalentAndModifierMaskFromAccelerator(*accelerator)
              : nil;
      for (NSMenuItem* item : menu_items) {
        item.keyEquivalent = equivalent ? equivalent.keyEquivalent : @"";
        item.keyEquivalentModifierMask =
            equivalent ? equivalent.modifierMask : 0;
      }
    }
  }

  const std::vector<NSMenuItem*>& custom_items() const {
    return custom_items_;
  }

  // ShortcutSettings::Observer:
  void OnShortcutsChanged() override { Apply(); }

 private:
  std::vector<NSMenuItem*> custom_items_;
};

}  // namespace

void AdjustMenuAccelerators(std::map<int, ui::Accelerator>& accelerators) {
  accelerators.erase(IDC_SAVE_PAGE);
  // Cmd+Option+N is a new little window's (AdjustShortcutsNotInMenu()).
  // Compass splits by dragging a tab onto another, or from its menu.
  accelerators.erase(IDC_NEW_SPLIT_TAB);
  // In place of Cmd+Shift+L. The View menu's item shows it.
  accelerators[IDC_TOGGLE_VERTICAL_TABS_COLLAPSE] =
      ui::Accelerator(ui::VKEY_S, ui::EF_COMMAND_DOWN);

  // The singleton outlives everything that reads it.
  g_menu_accelerators = &accelerators;
  // Once the map and the main menu are built (BuildMainMenu() goes on to
  // build its items from it), the user's shortcuts go over them.
  ShortcutApplier::Get();
  if (base::SequencedTaskRunner::HasCurrentDefault()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce([] { ShortcutApplier::Get().Apply(); }));
  }
}

void AdjustShortcutsNotInMenu(std::vector<KeyboardShortcutData>& shortcuts) {
  for (KeyboardShortcutData& shortcut : shortcuts) {
    if (shortcut.command_key && shortcut.shift_key && !shortcut.cntrl_key &&
        !shortcut.opt_key && shortcut.vkey_code == kVK_ANSI_C) {
      // Was inspect element.
      shortcut.chrome_command = IDC_COPY_URL;
    }
  }
  shortcuts.push_back({/*command_key=*/true, /*shift_key=*/false,
                       /*cntrl_key=*/false, /*opt_key=*/true, kVK_ANSI_N,
                       kNewLittleWindowCommand});
  shortcuts.push_back({/*command_key=*/false, /*shift_key=*/true,
                       /*cntrl_key=*/true, /*opt_key=*/false, kVK_ANSI_M,
                       kToggleTabMuteCommand});
}

base::span<const CustomizableCommand> GetCustomizableCommands() {
  return kCustomizableCommands;
}

const CustomizableCommand* FindCustomizableCommand(std::string_view id) {
  for (const CustomizableCommand& entry : kCustomizableCommands) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

std::vector<ui::Accelerator> GetDefaultAccelerators(int command) {
  std::vector<ui::Accelerator> accelerators;
  const std::map<int, ui::Accelerator>& defaults = DefaultMenuAccelerators();
  if (auto it = defaults.find(command); it != defaults.end()) {
    accelerators.push_back(it->second);
  }
  for (const KeyboardShortcutData& shortcut :
       GetShortcutsNotPresentInMainMenu()) {
    if (shortcut.chrome_command != command) {
      continue;
    }
    const ui::Accelerator accelerator = AcceleratorFromShortcut(shortcut);
    if (!std::ranges::contains(accelerators, accelerator)) {
      accelerators.push_back(accelerator);
    }
  }
  return accelerators;
}

std::vector<ui::Accelerator> GetAccelerators(int command) {
  std::optional<ui::Accelerator> custom = CustomAcceleratorFor(command);
  if (!custom) {
    return GetDefaultAccelerators(command);
  }
  if (custom->key_code() == ui::VKEY_UNKNOWN) {
    return {};
  }
  return {*custom};
}

std::optional<std::u16string> FindBuiltInShortcut(
    const ui::Accelerator& accelerator) {
  KeyEquivalentAndModifierMask* equivalent =
      ui::GetKeyEquivalentAndModifierMaskFromAccelerator(accelerator);
  std::optional<std::u16string> name;
  ForEachMenuItem(NSApp.mainMenu, [&](NSMenuItem* item) {
    if (!name && !IsCustomizable(static_cast<int>(item.tag)) &&
        item.title.length &&
        [item.keyEquivalent isEqualToString:equivalent.keyEquivalent] &&
        item.keyEquivalentModifierMask == equivalent.modifierMask) {
      name = base::SysNSStringToUTF16(item.title);
    }
  });
  if (name) {
    return name;
  }
  for (const KeyboardShortcutData& shortcut :
       GetShortcutsNotPresentInMainMenu()) {
    if (!IsCustomizable(shortcut.chrome_command) &&
        AcceleratorFromShortcut(shortcut) == accelerator) {
      return u"a built-in shortcut";
    }
  }
  return std::nullopt;
}

bool IsRecordingShortcut() {
  return ShortcutSettings::Get().recording();
}

std::optional<int> CustomCommandForKeyEvent(NSEvent* event) {
  for (NSMenuItem* item : ShortcutApplier::Get().custom_items()) {
    if ([item cr_firesForKeyEquivalentEvent:event]) {
      return static_cast<int>(item.tag);
    }
  }
  // A default of a command the user gave other keys (or none).
  for (NSMenuItem* item : UpstreamItemsNotInMenu()) {
    if (CustomAcceleratorFor(static_cast<int>(item.tag)) &&
        [item cr_firesForKeyEquivalentEvent:event]) {
      return NO_COMMAND;
    }
  }
  return std::nullopt;
}

std::optional<bool> CustomAcceleratorForCommand(int command,
                                                ui::Accelerator* accelerator) {
  std::optional<ui::Accelerator> custom = CustomAcceleratorFor(command);
  if (!custom) {
    return std::nullopt;
  }
  if (custom->key_code() == ui::VKEY_UNKNOWN) {
    return false;
  }
  *accelerator = *custom;
  return true;
}

}  // namespace compass
