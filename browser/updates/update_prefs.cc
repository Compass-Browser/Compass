// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/updates/update_prefs.h"

#include "compass/browser/updates/update_hooks.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"

namespace compass::updates {

void RegisterUpdatePrefs(PrefRegistrySimple* registry) {
  registry->RegisterStringPref(kUpdateChannelPref, kChannelStable);
}

bool IsBetaChannel(const PrefService& local_state) {
  return local_state.GetString(kUpdateChannelPref) == kChannelBeta;
}

}  // namespace compass::updates
