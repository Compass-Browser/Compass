// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UPDATES_START_UPDATER_H_
#define COMPASS_BROWSER_UPDATES_START_UPDATER_H_

namespace compass::updates {

// Starts this platform's updater: Sparkle on macOS, the manifest updater on
// Windows (with compass_enable_win_updater), nothing elsewhere. Called from
// ChromeBrowserMainParts::PostBrowserStart()
// (patches/chrome-browser-chrome_browser_main.cc.patch).
void StartUpdater();

}  // namespace compass::updates

#endif  // COMPASS_BROWSER_UPDATES_START_UPDATER_H_
