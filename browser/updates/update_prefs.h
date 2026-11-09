// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UPDATES_UPDATE_PREFS_H_
#define COMPASS_BROWSER_UPDATES_UPDATE_PREFS_H_

#include <string_view>

class PrefRegistrySimple;
class PrefService;

namespace compass::updates {

// Local State prefs: the update channel (kUpdateChannelPref, "stable").
void RegisterUpdatePrefs(PrefRegistrySimple* registry);

// The channel |local_state| picks: kChannelStable, kChannelBeta or
// kChannelCanary. Anything else counts as stable.
std::string_view UpdateChannel(const PrefService& local_state);

}  // namespace compass::updates

#endif  // COMPASS_BROWSER_UPDATES_UPDATE_PREFS_H_
