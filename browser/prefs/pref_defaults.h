// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_PREFS_PREF_DEFAULTS_H_
#define COMPASS_BROWSER_PREFS_PREF_DEFAULTS_H_

class PrefRegistrySimple;

namespace compass {

// Compass's defaults for upstream prefs, set once upstream has registered
// them (chrome/browser/prefs/browser_prefs.cc calls these last, see
// patches/). A default only applies while the user hasn't changed the
// setting, and policy still wins. docs/design/privacy.md.

// Local State: no profile picker at startup.
void OverrideLocalStatePrefDefaults(PrefRegistrySimple* registry);

// Profile prefs: no password leak check, no Google-backed error page
// probes, no browser sign-in. Takes the profile registry (a PrefRegistrySimple subclass).
void OverrideProfilePrefDefaults(PrefRegistrySimple* registry);

}  // namespace compass

#endif  // COMPASS_BROWSER_PREFS_PREF_DEFAULTS_H_
