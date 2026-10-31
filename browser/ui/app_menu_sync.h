// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_APP_MENU_SYNC_H_
#define COMPASS_BROWSER_UI_APP_MENU_SYNC_H_

#include <optional>
#include <string>

class BrowserWindowInterface;

// The sync row of the header's app menu (docs/design/sync.md, "Slice 4
// notes"). Upstream's AppMenuModel builds the menu each time it opens and
// asks for the row there (patches/
// chrome-browser-ui-toolbar-app_menu_model.cc.patch); the row's command is
// compass::kOpenSyncPageCommand (compass_command_ids.h).
//
// Declarations only, in //compass/browser/ui:app_menu_hooks, so upstream's
// toolbar target can include them without depending on Compass's UI; the
// definitions are in //compass/browser/ui, linked with //chrome/browser/ui.
namespace compass {

// The row's label ("Synced", "Sync error"...), or nullopt for a profile
// that can't sync (incognito, guest): no row.
std::optional<std::u16string> GetAppMenuSyncLabel(
    BrowserWindowInterface* browser);

// Opens chrome://compass-sync for kOpenSyncPageCommand; false for other
// commands.
bool HandleAppMenuSyncCommand(BrowserWindowInterface* browser, int id);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_APP_MENU_SYNC_H_
