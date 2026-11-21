// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_SETTINGS_WINDOW_HOOKS_H_
#define COMPASS_BROWSER_UI_SETTINGS_WINDOW_HOOKS_H_

#include <string_view>

class Profile;

namespace content {
class NavigationThrottleRegistry;
}  // namespace content

// Settings open in Compass's settings window (docs/design/settings-window.md)
// instead of a chrome://settings tab. Upstream's
// chrome::ShowSettingsSubPageInTabbedBrowser(), which every way of opening
// settings ends in (IDC_OPTIONS, Cmd+, and the menus,
// chrome::ShowSettingsSubPage()), asks here first
// (see patches/).
//
// A declaration only, in //compass/browser/ui:app_menu_hooks, so upstream
// targets can include it without depending on Compass's UI; the definition
// is in //compass/browser/ui (views/settings_window/).
namespace compass {

// Shows |sub_page| ("", "sync", "boosts?add=example.com") in |profile|'s
// settings window and returns true if the window has it; false for
// upstream's sections, which the window doesn't have yet.
bool MaybeShowSettingsWindow(Profile* profile, std::string_view sub_page);

// A tab loading settings the window has opens the window instead
// (SettingsWindowNavigationThrottle). Called by upstream's
// CreateAndAddChromeThrottlesForNavigation() (see patches/).
void MaybeAddSettingsWindowThrottle(
    content::NavigationThrottleRegistry& registry);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_SETTINGS_WINDOW_HOOKS_H_
