// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_PREFS_COMPASS_PREFS_H_
#define COMPASS_BROWSER_PREFS_COMPASS_PREFS_H_

class PrefRegistrySimple;

namespace compass {

namespace prefs {

// A profile imported from Arc: Arc's profile folder it came from ("Default",
// "Profile 1", ...). Written with the copied preferences, before the profile
// first loads (docs/design/arc-import.md).
inline constexpr char kArcImportProfileDir[] =
    "compass.arc_import.profile_dir";

// The Arc space IDs imported into this profile, so importing again only
// offers Arc's newer spaces (docs/design/arc-import.md, "Importing again").
inline constexpr char kArcImportSpaces[] = "compass.arc_import.spaces";

}  // namespace prefs

// Compass's own profile prefs. Called by upstream's RegisterProfilePrefs()
// (patches/chrome-browser-prefs-browser_prefs.cc.patch).
void RegisterCompassProfilePrefs(PrefRegistrySimple* registry);

}  // namespace compass

#endif  // COMPASS_BROWSER_PREFS_COMPASS_PREFS_H_
