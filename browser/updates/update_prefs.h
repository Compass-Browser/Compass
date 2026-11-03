// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UPDATES_UPDATE_PREFS_H_
#define COMPASS_BROWSER_UPDATES_UPDATE_PREFS_H_

class PrefRegistrySimple;
class PrefService;

namespace compass::updates {

// Local State prefs: the update channel (kUpdateChannelPref, "stable").
void RegisterUpdatePrefs(PrefRegistrySimple* registry);

// Whether |local_state| picks the beta channel. Anything that isn't "beta"
// counts as stable.
bool IsBetaChannel(const PrefService& local_state);

}  // namespace compass::updates

#endif  // COMPASS_BROWSER_UPDATES_UPDATE_PREFS_H_
