// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/prefs/compass_prefs.h"

#include "base/values.h"
#include "compass/browser/claude/claude_prefs.h"
#include "components/prefs/pref_registry_simple.h"

namespace compass {

void RegisterCompassProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterStringPref(prefs::kArcImportProfileDir, std::string());
  registry->RegisterListPref(prefs::kArcImportSpaces);
  registry->RegisterStringPref(prefs::kBrowserImportProfile, std::string());
  registry->RegisterBooleanPref(prefs::kPipOnLeavingWindow, true);
  registry->RegisterBooleanPref(prefs::kRemoveTrackingFromCopiedLinks, true);
  // On for local development servers until the user takes them off
  // (docs/design/dev-mode.md, "The sites").
  registry->RegisterListPref(
      prefs::kDevModeHosts,
      base::ListValue().Append(prefs::kDevModeDefaultHost));
  registry->RegisterListPref(prefs::kPresentAsSafariSites);
  registry->RegisterBooleanPref(prefs::kTabPreviewsEnabled, true);
  registry->RegisterBooleanPref(prefs::kCtrlTabRecent, true);
  registry->RegisterDictionaryPref(prefs::kTabPreviewPlugins);
  claude::prefs::RegisterProfilePrefs(registry);
  // Sharing: "Your name for sharing" (compass::rooms::kDisplayNamePref,
  // browser/rooms/room_service.h). Here rather than with RoomService's
  // factory, which is built after profiles' prefs are.
  registry->RegisterStringPref("compass.rooms.display_name", std::string());
}

void MigrateCompassProfilePrefs(PrefService* prefs) {
  // Other AI providers' settings (docs/design/ai-providers.md, "Settings").
  claude::prefs::MigrateProviderPrefs(prefs);
}

}  // namespace compass
