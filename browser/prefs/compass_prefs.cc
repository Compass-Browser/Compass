// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/prefs/compass_prefs.h"

#include "components/prefs/pref_registry_simple.h"

namespace compass {

void RegisterCompassProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterStringPref(prefs::kArcImportProfileDir, std::string());
  registry->RegisterListPref(prefs::kArcImportSpaces);
}

}  // namespace compass
