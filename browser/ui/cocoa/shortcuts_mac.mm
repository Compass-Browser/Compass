// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/cocoa/shortcuts_mac.h"

#include <Carbon/Carbon.h>

#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/global_keyboard_shortcuts_mac.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace compass {

void AdjustMenuAccelerators(std::map<int, ui::Accelerator>& accelerators) {
  accelerators.erase(IDC_SAVE_PAGE);
  // In place of Cmd+Shift+L. The View menu's item shows it.
  accelerators[IDC_TOGGLE_VERTICAL_TABS_COLLAPSE] =
      ui::Accelerator(ui::VKEY_S, ui::EF_COMMAND_DOWN);
}

void AdjustShortcutsNotInMenu(std::vector<KeyboardShortcutData>& shortcuts) {
  for (KeyboardShortcutData& shortcut : shortcuts) {
    if (shortcut.command_key && shortcut.shift_key && !shortcut.cntrl_key &&
        !shortcut.opt_key && shortcut.vkey_code == kVK_ANSI_C) {
      // Was inspect element.
      shortcut.chrome_command = IDC_COPY_URL;
    }
  }
}

}  // namespace compass
