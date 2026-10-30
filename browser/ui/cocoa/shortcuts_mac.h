// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_COCOA_SHORTCUTS_MAC_H_
#define STRATA_BROWSER_UI_COCOA_SHORTCUTS_MAC_H_

#include <map>
#include <vector>

#include "ui/base/accelerators/accelerator.h"

struct KeyboardShortcutData;

namespace strata {

// Strata's shortcuts over upstream's, as in Arc (docs/design/shortcuts.md):
// Cmd+Shift+C copies the page's link (upstream's inspect element stays on
// Cmd+Option+C) and Cmd+S opens or closes the sidebar (upstream's Save Page
// As keeps its menu item, without a shortcut). Cmd+Shift+R reloads without
// the cache, as upstream.

// The main menu's key equivalents, by command (AcceleratorsCocoa).
void AdjustMenuAccelerators(std::map<int, ui::Accelerator>& accelerators);

// The shortcuts that aren't in the main menu
// (GetShortcutsNotPresentInMainMenu()).
void AdjustShortcutsNotInMenu(std::vector<KeyboardShortcutData>& shortcuts);

}  // namespace strata

#endif  // STRATA_BROWSER_UI_COCOA_SHORTCUTS_MAC_H_
