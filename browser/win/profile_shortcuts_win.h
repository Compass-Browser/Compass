// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_WIN_PROFILE_SHORTCUTS_WIN_H_
#define COMPASS_BROWSER_WIN_PROFILE_SHORTCUTS_WIN_H_

namespace compass::win {

// Compass is one app on Windows, whatever the profile, as on macOS: one
// taskbar group (patches/chrome-browser-shell_integration_win.cc.patch) and
// one desktop shortcut, with Chromium's per-profile desktop shortcuts off
// (patches/chrome-browser-profiles-profile_shortcut_manager_win.cc.patch).
// Before that, a second profile renamed the desktop shortcut after the
// first ("Your Chromium - Compass") and added one per profile. Those go, in
// the background: shortcuts on the user's desktop to this chrome.exe with a
// --profile-directory, named "<profile> - <product>". If any went and no
// plain shortcut is left, the plain one is made again. Idempotent; run at
// each launch.
void RemoveProfileDesktopShortcuts();

}  // namespace compass::win

#endif  // COMPASS_BROWSER_WIN_PROFILE_SHORTCUTS_WIN_H_
