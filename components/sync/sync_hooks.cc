// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/sync_hooks.h"

#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/logging.h"
#include "compass/components/sync/engine_components_factory.h"
#include "compass/components/sync/pref_names.h"
#include "compass/components/sync/sync_credentials.h"
#include "components/prefs/pref_service.h"
#include "components/sync/base/command_line_switches.h"
#include "url/gurl.h"

namespace compass::sync {
namespace {

// Empty if |name| isn't registered (tests that build a SyncServiceImpl on
// bare prefs).
std::string GetStringPref(const PrefService* prefs, const char* name) {
  return prefs->FindPreference(name) ? prefs->GetString(name) : std::string();
}

}  // namespace

GURL GetServerUrl(const PrefService* prefs, const GURL& upstream_url) {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          syncer::kSyncServiceURL)) {
    return upstream_url;
  }
  GURL url(GetStringPref(prefs, prefs::kServerUrl));
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS() ? url : GURL();
}

bool IsChainEnabled(const PrefService* prefs, const GURL& upstream_url) {
  return !GetStringPref(prefs, prefs::kSeed).empty() &&
         GetServerUrl(prefs, upstream_url).is_valid();
}

std::unique_ptr<syncer::EngineComponentsFactory> CreateEngineComponentsFactory(
    const PrefService* prefs,
    const syncer::EngineComponentsFactory::Switches& switches,
    const GURL& upstream_url,
    syncer::EngineComponentsFactoryImpl::HttpPostProviderFactoryGetter
        http_factory_getter) {
  const GURL url = GetServerUrl(prefs, upstream_url);
  VLOG(1) << "Compass sync: engine talks to " << url.spec();
  return std::make_unique<CompassEngineComponentsFactory>(
      switches, url, SyncCredentials::ForPrefs(prefs),
      std::move(http_factory_getter));
}

}  // namespace compass::sync
