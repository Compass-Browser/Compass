// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_COMPASS_COMMAND_IDS_H_
#define COMPASS_BROWSER_UI_COMPASS_COMMAND_IDS_H_

#include "chrome/app/chrome_command_ids.h"

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

// Cmd+Shift+M: mutes the active tab, or unmutes it (docs/design/sidebar.md,
// "Sound").
inline constexpr int kToggleTabMuteCommand = 39952;

// Cmd+Shift+E: shows or hides the chat with Claude (docs/design/claude.md,
// "Chat panel").
inline constexpr int kToggleClaudeCommand = 39953;

// Cmd+Z and Cmd+Shift+Z outside text: undo and redo closing sidebar entries
// (docs/design/sidebar.md, "Undo closing"). In text, they undo and redo the
// typing, as the Edit menu does.
inline constexpr int kUndoCloseTabCommand = 39954;
inline constexpr int kRedoCloseTabCommand = 39955;

inline bool IsCompassCommand(int id) {
  return id == kNewLittleWindowCommand || id == kOpenSyncPageCommand ||
         id == kToggleTabMuteCommand || id == kToggleClaudeCommand ||
         id == kUndoCloseTabCommand || id == kRedoCloseTabCommand;
}

// Upstream commands of features Compass removes: bookmarks and the reading
// list (pinned tabs are the bookmarks), horizontal tabs (the sidebar is the
// tab strip), tab search (the command bar finds tabs), and the app menu's
// profile row and tab groups menu (a space picks its profile; folders are
// its groups).
// BrowserCommandController reports them unsupported, so no menu, shortcut
// or caller can run them, and the menus leave them out.
inline bool IsRemovedCommand(int id) {
  switch (id) {
    case IDC_BOOKMARK_THIS_TAB:
    case IDC_BOOKMARK_ALL_TABS:
    case IDC_SHOW_BOOKMARK_BAR:
    case IDC_SHOW_BOOKMARK_MANAGER:
    case IDC_SHOW_BOOKMARK_SIDE_PANEL:
    case IDC_BOOKMARKS_MENU:
    case IDC_FOCUS_BOOKMARKS:
    case IDC_BOOKMARK_BAR_SUBMENU:
    case IDC_BOOKMARK_BAR_SUBMENU_ALWAYS_HIDE:
    case IDC_BOOKMARK_BAR_SUBMENU_ALWAYS_SHOW:
    case IDC_BOOKMARK_BAR_SUBMENU_ONLY_ON_NTP:
    case IDC_READING_LIST_MENU:
    case IDC_READING_LIST_MENU_ADD_TAB:
    case IDC_READING_LIST_MENU_SHOW_UI:
    case IDC_CONTENT_CONTEXT_ADD_LINK_TO_READING_LIST:
    case IDC_TOGGLE_VERTICAL_TABS:
    case IDC_TAB_SEARCH:
    case IDC_PROFILE_MENU_IN_APP_MENU:
    case IDC_SAVED_TAB_GROUPS_MENU:
      return true;
    default:
      return false;
  }
}

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_COMPASS_COMMAND_IDS_H_
