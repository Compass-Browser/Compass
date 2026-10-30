// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_COCOA_SHORTCUTS_MAC_H_
#define COMPASS_BROWSER_UI_COCOA_SHORTCUTS_MAC_H_

#include <map>
#include <vector>

#include "ui/base/accelerators/accelerator.h"

struct KeyboardShortcutData;

namespace compass {

// Compass's shortcuts over upstream's, as in Arc (docs/design/shortcuts.md):
// Cmd+Shift+C copies the page's link (upstream's inspect element stays on
// Cmd+Option+C) and Cmd+S opens or closes the sidebar (upstream's Save Page
// As keeps its menu item, without a shortcut). Cmd+Shift+R reloads without
// the cache, as upstream.

// The main menu's key equivalents, by command (AcceleratorsCocoa).
void AdjustMenuAccelerators(std::map<int, ui::Accelerator>& accelerators);

// The shortcuts that aren't in the main menu
// (GetShortcutsNotPresentInMainMenu()).
void AdjustShortcutsNotInMenu(std::vector<KeyboardShortcutData>& shortcuts);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_COCOA_SHORTCUTS_MAC_H_
