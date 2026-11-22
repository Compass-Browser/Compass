// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/adblock/adblock_prefs.h"

#include "components/prefs/pref_registry_simple.h"
#include "components/subresource_filter/core/browser/ruleset_version.h"

namespace compass::adblock {

void RegisterAdBlockLocalStatePrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(prefs::kEnabled, true);
  registry->RegisterListPref(prefs::kCustomLists);
  registry->RegisterListPref(prefs::kDisabledLists);
  registry->RegisterDictionaryPref(prefs::kListState);
  subresource_filter::IndexedRulesetVersion::RegisterPrefs(
      registry, kRulesetConfig.filter_tag);
}

}  // namespace compass::adblock
