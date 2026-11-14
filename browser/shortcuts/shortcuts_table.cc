// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/shortcuts/shortcuts_table.h"

#include <algorithm>
#include <map>

#include "base/no_destructor.h"
#include "chrome/app/chrome_command_ids.h"
#include "compass/browser/shortcuts/shortcut_settings.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace compass {

namespace {

// In the order the settings page shows them, by section: macOS's
// (shortcuts_mac.mm), less undo and redo of closing tabs.
constexpr CustomizableCommand kCustomizableCommands[] = {
    {"new-tab", IDC_NEW_TAB, "Compass", "New tab (command bar)"},
    {"open-location", IDC_FOCUS_LOCATION, "Compass",
     "Edit the page's address"},
    {"toggle-sidebar", IDC_TOGGLE_VERTICAL_TABS_COLLAPSE, "Compass",
     "Show or hide the sidebar"},
    {"copy-url", IDC_COPY_URL, "Compass", "Copy the page's link"},
    {"little-window", kNewLittleWindowCommand, "Compass",
     "New little window"},
    {"claude", kToggleClaudeCommand, "Compass", "Chat with Claude"},

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
    {"extensions", IDC_MANAGE_EXTENSIONS, "Browser", "Extensions"},
    {"settings", IDC_OPTIONS, "Browser", "Settings"},
    {"clear-browsing-data", IDC_CLEAR_BROWSING_DATA, "Browser",
     "Clear browsing data"},

    {"devtools", IDC_DEV_TOOLS, "Developer", "Developer tools"},
    {"console", IDC_DEV_TOOLS_CONSOLE, "Developer", "JavaScript console"},
    {"inspect", IDC_DEV_TOOLS_INSPECT, "Developer", "Inspect elements"},
    {"view-source", IDC_VIEW_SOURCE, "Developer", "View source"},
};

// Compass's keys over upstream's, as on macOS (Ctrl for Cmd): whatever
// upstream ran on them loses them, and the command loses upstream's own.
struct CompassDefault {
  ui::KeyboardCode key;
  int modifiers;
  int command;
};
constexpr CompassDefault kCompassDefaults[] = {
    // Was inspect element, which keeps F12's Developer tools and
    // Ctrl+Shift+I's.
    {ui::VKEY_C, ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN, IDC_COPY_URL},
    // Was Save Page As, which keeps its menu item.
    {ui::VKEY_S, ui::EF_CONTROL_DOWN, IDC_TOGGLE_VERTICAL_TABS_COLLAPSE},
    // Was the profile menu.
    {ui::VKEY_M, ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN,
     kToggleTabMuteCommand},
    {ui::VKEY_E, ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN,
     kToggleClaudeCommand},
};

struct Tables {
  // Each customizable command's keys as Compass ships them.
  std::map<int, std::vector<ui::Accelerator>> defaults;
  bool defaults_taken = false;
  // The table as last adjusted, the user's keys in it.
  std::vector<CommandAccelerator> current;
};

Tables& GetTables() {
  static base::NoDestructor<Tables> tables;
  return *tables;
}

bool IsCustomizable(int command) {
  return std::ranges::any_of(kCustomizableCommands,
                             [command](const CustomizableCommand& entry) {
                               return entry.command == command;
                             });
}

// |command| runs on |accelerator|, and nothing else does.
void Assign(std::vector<CommandAccelerator>& list,
            const ui::Accelerator& accelerator,
            int command) {
  std::erase_if(list, [&](const CommandAccelerator& entry) {
    return entry.accelerator == accelerator;
  });
  list.push_back({accelerator, command});
}

}  // namespace

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

void AdjustAcceleratorList(std::vector<CommandAccelerator>& list) {
  for (const CompassDefault& entry : kCompassDefaults) {
    std::erase_if(list, [&](const CommandAccelerator& mapping) {
      return mapping.command == entry.command;
    });
    Assign(list, ui::Accelerator(entry.key, entry.modifiers), entry.command);
  }

  Tables& tables = GetTables();
  if (!tables.defaults_taken) {
    tables.defaults_taken = true;
    for (const CommandAccelerator& entry : list) {
      if (IsCustomizable(entry.command)) {
        tables.defaults[entry.command].push_back(entry.accelerator);
      }
    }
  }

  const ShortcutSettings& settings = ShortcutSettings::Get();
  for (const CustomizableCommand& command : kCustomizableCommands) {
    const std::optional<ui::Accelerator> custom =
        settings.GetCustom(command.id);
    if (!custom) {
      continue;
    }
    std::erase_if(list, [&](const CommandAccelerator& entry) {
      return entry.command == command.command;
    });
    if (custom->key_code() != ui::VKEY_UNKNOWN) {
      Assign(list, *custom, command.command);
    }
  }
  tables.current = list;
}

std::vector<ui::Accelerator> GetDefaultAccelerators(int command) {
  const Tables& tables = GetTables();
  auto it = tables.defaults.find(command);
  return it == tables.defaults.end() ? std::vector<ui::Accelerator>()
                                     : it->second;
}

std::vector<ui::Accelerator> GetAccelerators(int command) {
  for (const CustomizableCommand& entry : kCustomizableCommands) {
    if (entry.command != command) {
      continue;
    }
    if (std::optional<ui::Accelerator> custom =
            ShortcutSettings::Get().GetCustom(entry.id)) {
      if (custom->key_code() == ui::VKEY_UNKNOWN) {
        return {};
      }
      return {*custom};
    }
    break;
  }
  return GetDefaultAccelerators(command);
}

std::optional<std::u16string> FindBuiltInShortcut(
    const ui::Accelerator& accelerator) {
  for (const CommandAccelerator& entry : GetTables().current) {
    if (entry.accelerator == accelerator && !IsCustomizable(entry.command)) {
      return u"another browser shortcut";
    }
  }
  return std::nullopt;
}

}  // namespace compass
