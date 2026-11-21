// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/prefs/compass_prefs.h"

#include "compass/browser/claude/claude_prefs.h"
#include "components/prefs/pref_registry_simple.h"

namespace compass {

void RegisterCompassProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterStringPref(prefs::kArcImportProfileDir, std::string());
  registry->RegisterListPref(prefs::kArcImportSpaces);
  registry->RegisterBooleanPref(prefs::kPipOnLeavingWindow, true);
  registry->RegisterBooleanPref(prefs::kRemoveTrackingFromCopiedLinks, true);
  claude::prefs::RegisterProfilePrefs(registry);
  // Sharing: "Your name for sharing" (compass::rooms::kDisplayNamePref,
  // browser/rooms/room_service.h). Here rather than with RoomService's
  // factory, which is built after profiles' prefs are.
  registry->RegisterStringPref("compass.rooms.display_name", std::string());
}

}  // namespace compass
