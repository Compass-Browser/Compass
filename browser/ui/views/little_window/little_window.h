// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_H_
#define COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_H_

#include <vector>

class BrowserWindowInterface;
class GURL;
class Profile;
struct StartupTab;

namespace compass {

// Little windows (Arc's Little Arc, docs/design/little-window.md): a link
// another app opens shows in a small window of its own, a popup-type
// `Browser` with a back button and "Open in <space>" (Cmd+O), which moves
// the page into the last active window's space as a Today entry.

// Opens each of |tabs| in a little window of |profile|, if they are all web
// pages or files; otherwise opens none and returns false. Called from
// OpenStartupTabsInBrowserWithProfile() (app_controller_mac.mm, see
// patches/), where macOS hands over URLs from other apps.
bool OpenInLittleWindows(const std::vector<StartupTab>& tabs,
                         Profile* profile);

// Opens |url| in a new little window of |profile| and returns it.
BrowserWindowInterface* OpenLittleWindow(Profile* profile, const GURL& url);

// Whether |browser| is a little window.
bool IsLittleWindow(BrowserWindowInterface* browser);

// Moves |browser|'s page into |profile|'s last active window (a new one if
// there's none) as the active tab; |browser| then closes. Returns the window
// it went to, or null if |browser| isn't a little window.
BrowserWindowInterface* OpenLittleWindowInSpace(BrowserWindowInterface* browser);

// IDC_OPEN_FILE (Cmd+O) in a little window opens it in a space. Returns true
// if it handled |id|. Called from compass::HandleBrowserCommand().
bool HandleLittleWindowCommand(BrowserWindowInterface* browser, int id);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_H_
