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
#include "compass/strings/grit/compass_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace compass {

namespace {

// In the order the settings page shows them, by section: macOS's
// (shortcuts_mac.mm), less undo and redo of closing tabs.
constexpr CustomizableCommand kCustomizableCommands[] = {
    {"new-tab", IDC_NEW_TAB, IDS_COMPASS_SHORTCUTS_SECTION_COMPASS,
     IDS_COMPASS_SHORTCUTS_NEW_TAB},
    {"open-location", IDC_FOCUS_LOCATION, IDS_COMPASS_SHORTCUTS_SECTION_COMPASS,
     IDS_COMPASS_SHORTCUTS_OPEN_LOCATION},
    {"toggle-sidebar", IDC_TOGGLE_VERTICAL_TABS_COLLAPSE,
     IDS_COMPASS_SHORTCUTS_SECTION_COMPASS,
     IDS_COMPASS_SHORTCUTS_TOGGLE_SIDEBAR},
    {"copy-url", IDC_COPY_URL, IDS_COMPASS_SHORTCUTS_SECTION_COMPASS,
     IDS_COMPASS_SHORTCUTS_COPY_URL},
    {"little-window", kNewLittleWindowCommand,
     IDS_COMPASS_SHORTCUTS_SECTION_COMPASS,
     IDS_COMPASS_SHORTCUTS_LITTLE_WINDOW},
    {"claude", kToggleClaudeCommand, IDS_COMPASS_SHORTCUTS_SECTION_COMPASS,
     IDS_COMPASS_SHORTCUTS_CLAUDE},
    {"reader", kToggleReaderModeCommand, IDS_COMPASS_SHORTCUTS_SECTION_COMPASS,
     IDS_COMPASS_SHORTCUTS_READER},

    {"restore-tab", IDC_RESTORE_TAB, IDS_COMPASS_SHORTCUTS_SECTION_TABS,
     IDS_COMPASS_SHORTCUTS_RESTORE_TAB},
    {"next-tab", IDC_SELECT_NEXT_TAB, IDS_COMPASS_SHORTCUTS_SECTION_TABS,
     IDS_COMPASS_SHORTCUTS_NEXT_TAB},
    {"previous-tab", IDC_SELECT_PREVIOUS_TAB,
     IDS_COMPASS_SHORTCUTS_SECTION_TABS, IDS_COMPASS_SHORTCUTS_PREVIOUS_TAB},
    {"mute-tab", kToggleTabMuteCommand, IDS_COMPASS_SHORTCUTS_SECTION_TABS,
     IDS_COMPASS_SHORTCUTS_MUTE_TAB},
    {"reload", IDC_RELOAD, IDS_COMPASS_SHORTCUTS_SECTION_TABS,
     IDS_COMPASS_SHORTCUTS_RELOAD},
    {"hard-reload", IDC_RELOAD_BYPASSING_CACHE,
     IDS_COMPASS_SHORTCUTS_SECTION_TABS, IDS_COMPASS_SHORTCUTS_HARD_RELOAD},
    {"back", IDC_BACK, IDS_COMPASS_SHORTCUTS_SECTION_TABS,
     IDS_COMPASS_SHORTCUTS_BACK},
    {"forward", IDC_FORWARD, IDS_COMPASS_SHORTCUTS_SECTION_TABS,
     IDS_COMPASS_SHORTCUTS_FORWARD},
    {"find", IDC_FIND, IDS_COMPASS_SHORTCUTS_SECTION_TABS,
     IDS_COMPASS_SHORTCUTS_FIND},
    {"zoom-in", IDC_ZOOM_PLUS, IDS_COMPASS_SHORTCUTS_SECTION_TABS,
     IDS_COMPASS_SHORTCUTS_ZOOM_IN},
    {"zoom-out", IDC_ZOOM_MINUS, IDS_COMPASS_SHORTCUTS_SECTION_TABS,
     IDS_COMPASS_SHORTCUTS_ZOOM_OUT},
    {"zoom-reset", IDC_ZOOM_NORMAL, IDS_COMPASS_SHORTCUTS_SECTION_TABS,
     IDS_COMPASS_SHORTCUTS_ZOOM_RESET},
    {"print", IDC_PRINT, IDS_COMPASS_SHORTCUTS_SECTION_TABS,
     IDS_COMPASS_SHORTCUTS_PRINT},
    {"save-page", IDC_SAVE_PAGE, IDS_COMPASS_SHORTCUTS_SECTION_TABS,
     IDS_COMPASS_SHORTCUTS_SAVE_PAGE},

    {"new-window", IDC_NEW_WINDOW, IDS_COMPASS_SHORTCUTS_SECTION_WINDOWS,
     IDS_COMPASS_SHORTCUTS_NEW_WINDOW},
    {"new-incognito-window", IDC_NEW_INCOGNITO_WINDOW,
     IDS_COMPASS_SHORTCUTS_SECTION_WINDOWS,
     IDS_COMPASS_SHORTCUTS_NEW_INCOGNITO_WINDOW},
    {"fullscreen", IDC_FULLSCREEN, IDS_COMPASS_SHORTCUTS_SECTION_WINDOWS,
     IDS_COMPASS_SHORTCUTS_FULLSCREEN},

    {"history", IDC_SHOW_HISTORY, IDS_COMPASS_SHORTCUTS_SECTION_BROWSER,
     IDS_COMPASS_SHORTCUTS_HISTORY},
    {"downloads", IDC_SHOW_DOWNLOADS, IDS_COMPASS_SHORTCUTS_SECTION_BROWSER,
     IDS_COMPASS_SHORTCUTS_DOWNLOADS},
    {"extensions", IDC_MANAGE_EXTENSIONS, IDS_COMPASS_SHORTCUTS_SECTION_BROWSER,
     IDS_COMPASS_SHORTCUTS_EXTENSIONS},
    {"settings", IDC_OPTIONS, IDS_COMPASS_SHORTCUTS_SECTION_BROWSER,
     IDS_COMPASS_SHORTCUTS_SETTINGS},
    {"clear-browsing-data", IDC_CLEAR_BROWSING_DATA,
     IDS_COMPASS_SHORTCUTS_SECTION_BROWSER,
     IDS_COMPASS_SHORTCUTS_CLEAR_BROWSING_DATA},

    {"devtools", IDC_DEV_TOOLS, IDS_COMPASS_SHORTCUTS_SECTION_DEVELOPER,
     IDS_COMPASS_SHORTCUTS_DEVTOOLS},
    {"console", IDC_DEV_TOOLS_CONSOLE, IDS_COMPASS_SHORTCUTS_SECTION_DEVELOPER,
     IDS_COMPASS_SHORTCUTS_CONSOLE},
    {"inspect", IDC_DEV_TOOLS_INSPECT, IDS_COMPASS_SHORTCUTS_SECTION_DEVELOPER,
     IDS_COMPASS_SHORTCUTS_INSPECT},
    {"view-source", IDC_VIEW_SOURCE, IDS_COMPASS_SHORTCUTS_SECTION_DEVELOPER,
     IDS_COMPASS_SHORTCUTS_VIEW_SOURCE},
    {"dev-mode", kToggleDevModeCommand, IDS_COMPASS_SHORTCUTS_SECTION_DEVELOPER,
     IDS_COMPASS_SHORTCUTS_DEV_MODE},
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
    // Was upstream's Reading mode, which Compass's Reader replaces.
    {ui::VKEY_R, ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN,
     kToggleReaderModeCommand},
    // Was Bookmark All Tabs: Compass has no bookmarks.
    {ui::VKEY_D, ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN,
     kToggleDevModeCommand},
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
      return l10n_util::GetStringUTF16(IDS_COMPASS_SHORTCUTS_IN_USE_BY_BROWSER);
    }
  }
  return std::nullopt;
}

}  // namespace compass
