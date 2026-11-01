// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_WINDOW_SYNC_STARTUP_SPACE_H_
#define COMPASS_BROWSER_WINDOW_SYNC_STARTUP_SPACE_H_

class BrowserWindowInterface;

namespace compass {

// With spaces of several profiles there's no profile picker at startup
// (compass/browser/prefs/pref_defaults.cc): the first window opens in the
// last used profile, and once every listed profile's spaces are in, it shows
// the switcher's first space, swapping to that space's profile if needed.
// Only for the process's first normal window, and not when the session is
// restored (it brings back the spaces the windows showed). See
// docs/design/spaces.md, "Startup with several profiles".
//
// Called by WindowFeatures for each window it's created for.
void MaybeShowFirstSpaceAtStartup(BrowserWindowInterface* browser);

// Tests: whether the next first window is considered (default true).
void SetShowFirstSpaceAtStartupForTesting(bool enabled);

}  // namespace compass

#endif  // COMPASS_BROWSER_WINDOW_SYNC_STARTUP_SPACE_H_
