// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_COMPASS_COMMAND_IDS_H_
#define COMPASS_BROWSER_UI_COMPASS_COMMAND_IDS_H_

namespace compass {

// Browser commands of Compass's own, beside upstream's
// (chrome/app/chrome_command_ids.h), in a range upstream doesn't use. They
// aren't in upstream's command updater (which, with browser actions on, only
// knows commands that have an action): BrowserCommandController hands them
// to compass::HandleBrowserCommand() (command_bar.h) first, and they're
// always enabled.

// Cmd+Option+N: a new little window (docs/design/little-window.md).
inline constexpr int kNewLittleWindowCommand = 39950;

// The app menu's sync row: opens chrome://compass-sync (app_menu_sync.h).
inline constexpr int kOpenSyncPageCommand = 39951;

inline bool IsCompassCommand(int id) {
  return id == kNewLittleWindowCommand || id == kOpenSyncPageCommand;
}

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_COMPASS_COMMAND_IDS_H_
