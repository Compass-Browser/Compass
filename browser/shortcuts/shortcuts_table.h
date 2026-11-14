// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SHORTCUTS_SHORTCUTS_TABLE_H_
#define COMPASS_BROWSER_SHORTCUTS_SHORTCUTS_TABLE_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "ui/base/accelerators/accelerator.h"

namespace compass {

// Compass's shortcuts and the user's on Windows and Linux
// (docs/design/shortcuts.md, "Windows and Linux"). The browser's shortcuts
// are upstream's accelerator table (GetAcceleratorList(),
// accelerator_table.cc), which every window registers with its focus manager
// (BrowserView::LoadAccelerators()) and its menus show; a hook at the
// table's end hands it to AdjustAcceleratorList().
// The same API as shortcuts_mac.h, for the settings page.

// The browser commands whose shortcuts the settings page lists and the user
// can change, as on macOS (the same ids in kShortcutsPref), without undo and
// redo of closing tabs (Ctrl+Z, which only macOS runs after the page).
struct CustomizableCommand {
  // Its key in kShortcutsPref (shortcut_settings.h).
  std::string_view id;
  int command;
  std::string_view section;
  std::string_view label;
};
base::span<const CustomizableCommand> GetCustomizableCommands();
const CustomizableCommand* FindCustomizableCommand(std::string_view id);

// An entry of upstream's accelerator table.
struct CommandAccelerator {
  ui::Accelerator accelerator;
  int command = 0;
};

// The hook (see patches/): Compass's shortcuts go over upstream's (Ctrl+
// Shift+C copies the page's link, Ctrl+S opens or closes the sidebar,
// Ctrl+Shift+M mutes the tab, Ctrl+Shift+E chats with Claude), then the
// user's over those. The first call also keeps the result before the
// user's, as the defaults.
void AdjustAcceleratorList(std::vector<CommandAccelerator>& list);

// |command|'s shortcuts as Compass ships them, and the ones in force: the
// user's, if they changed them. Empty before the first window loaded the
// table.
std::vector<ui::Accelerator> GetDefaultAccelerators(int command);
std::vector<ui::Accelerator> GetAccelerators(int command);

// What upstream's table runs on |accelerator| that isn't customizable, if
// anything does: the page doesn't let a shortcut take its keys.
std::optional<std::u16string> FindBuiltInShortcut(
    const ui::Accelerator& accelerator);

}  // namespace compass

#endif  // COMPASS_BROWSER_SHORTCUTS_SHORTCUTS_TABLE_H_
