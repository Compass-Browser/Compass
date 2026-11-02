// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_prefs.h"

#include <string>

#include "components/prefs/pref_registry_simple.h"

namespace compass::claude::prefs {

void RegisterProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterStringPref(kApiKey, std::string());
  registry->RegisterStringPref(kApiKeyHint, std::string());
  registry->RegisterStringPref(kBaseUrl, std::string());
  registry->RegisterIntegerPref(kAuthStyle, 0);
  // claude_api.h's kDefaultModel; this target stays free of the client's
  // dependencies, as upstream's prefs registration reaches it.
  registry->RegisterStringPref(kModel, "claude-opus-5-5");
  registry->RegisterBooleanPref(kBrowserScope, false);
  registry->RegisterBooleanPref(kMcpEnabled, false);
  registry->RegisterListPref(kMcpAllowedClients);
}

}  // namespace compass::claude::prefs
