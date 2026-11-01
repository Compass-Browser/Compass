// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_SYNC_HOOKS_H_
#define COMPASS_COMPONENTS_SYNC_SYNC_HOOKS_H_

#include <memory>

#include "components/sync/engine/engine_components_factory.h"
#include "components/sync/engine/engine_components_factory_impl.h"

class GURL;
class PrefService;

// Called from SyncServiceImpl (see
// patches/components-sync-service-sync_service_impl.cc.patch).
namespace compass::sync {

// The server for |prefs|'s profile: --sync-url if given (then
// |upstream_url|, which already is it), else compass.sync.server_url. Empty
// if neither is set.
GURL GetServerUrl(const PrefService* prefs, const GURL& upstream_url);

// Whether local sync should talk to the chain's server: the profile has a
// seed and a server. Only asked when local sync is enabled.
bool IsChainEnabled(const PrefService* prefs, const GURL& upstream_url);

// The engine components for the chain's server. Only when IsChainEnabled().
std::unique_ptr<syncer::EngineComponentsFactory> CreateEngineComponentsFactory(
    const PrefService* prefs,
    const syncer::EngineComponentsFactory::Switches& switches,
    const GURL& upstream_url,
    syncer::EngineComponentsFactoryImpl::HttpPostProviderFactoryGetter
        http_factory_getter);

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_SYNC_HOOKS_H_
