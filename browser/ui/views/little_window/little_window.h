// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_H_
#define COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_H_

#include <memory>
#include <optional>
#include <vector>

#include "compass/browser/ui/compass_command_ids.h"

class BrowserWindowInterface;
class GURL;
class Profile;
struct StartupTab;

namespace gfx {
class Point;
}  // namespace gfx

namespace tabs {
class TabModel;
}  // namespace tabs

namespace views {
class View;
}  // namespace views

namespace compass {

class SpaceModel;

// Little windows (Arc's Little Arc, docs/design/little-window.md): a link
// another app opens shows in a small window of its own, a popup-type
// `Browser` with a back button and "Open in <space>" (Cmd+O), which moves
// the page into the last active window's space as a Today entry.

// Whether |tabs| (links from another app) would open in little windows: they
// are all web pages or files, and there is at least one.
bool CanOpenInLittleWindows(const std::vector<StartupTab>& tabs);

// Opens each of |tabs| in a little window of |profile|, if they are all web
// pages or files; otherwise opens none and returns false. Called where links
// from other apps arrive: OpenStartupTabsInBrowserWithProfile() on macOS
// (app_controller_mac.mm), StartupBrowserCreatorImpl on Windows, whose
// links come on the command line (see patches/).
bool OpenInLittleWindows(const std::vector<StartupTab>& tabs,
                         Profile* profile);

// Opens |url|, from another app, in a new little window of |profile| over
// that app's window, and returns it.
BrowserWindowInterface* OpenLittleWindow(Profile* profile, const GURL& url);

// Cmd+Option+N, Shift+Alt+N on Windows (kNewLittleWindowCommand,
// docs/design/shortcuts.md): a new little window on the new tab page, centered on |from|'s display.
BrowserWindowInterface* OpenNewLittleWindow(BrowserWindowInterface* from);

// A sidebar entry dragged out of the windows (docs/design/sidebar.md, "Drag
// and drop"): whether |point|, in screen coordinates, is outside every
// Compass window on screen.
bool IsOutsideCompassWindows(const gfx::Point& point);

// Opens |tab|, taken out of a window, in a new little window of |profile|
// with its bar under |point| (screen coordinates), and returns it.
BrowserWindowInterface* OpenTabInLittleWindowAt(
    Profile* profile,
    std::unique_ptr<tabs::TabModel> tab,
    const gfx::Point& point);

// The same for |url|, which opens in the new little window.
BrowserWindowInterface* OpenLittleWindowAt(Profile* profile,
                                           const GURL& url,
                                           const gfx::Point& point);

// Whether |browser| is a little window.
bool IsLittleWindow(BrowserWindowInterface* browser);

// Whether the window being created now is a little window: it has no native
// title bar, as a normal window (BrowserNativeWidgetMac, see patches/).
bool IsCreatingLittleWindow();

// The height of a little window's bar, which macOS centers the traffic
// lights on (BrowserNativeWidgetMac::GetWindowFrameTitlebarHeight(), see
// patches/), if |browser_view| is a little window's or is being created.
std::optional<int> GetLittleWindowBarHeight(views::View* browser_view);

// Whether |point| (in |browser_view|'s coordinates) is on a little window's
// frame or the empty part of its bar, which drag the window
// (BrowserView::NonClientHitTest(), see patches/).
bool IsOnLittleWindowCaption(views::View* browser_view,
                             const gfx::Point& point);

// Moves |browser|'s page into its profile's last active window (a new one if
// there's none) as the active tab; |browser| then closes. With |space| (one
// of the profile's spaces), into the last active window showing it, else the
// last active window switched to it. Returns the window it went to, or null
// if |browser| isn't a little window.
BrowserWindowInterface* OpenLittleWindowInSpace(BrowserWindowInterface* browser,
                                                SpaceModel* space = nullptr);

// IDC_OPEN_FILE (Cmd+O) in a little window opens it in a space;
// kNewLittleWindowCommand opens a little window. Returns true if it handled
// |id|. Called from compass::HandleBrowserCommand().
bool HandleLittleWindowCommand(BrowserWindowInterface* browser, int id);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_H_
