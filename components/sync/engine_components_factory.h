// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_ENGINE_COMPONENTS_FACTORY_H_
#define COMPASS_COMPONENTS_SYNC_ENGINE_COMPONENTS_FACTORY_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "components/sync/engine/engine_components_factory.h"
#include "components/sync/engine/engine_components_factory_impl.h"
#include "url/gurl.h"

namespace compass::sync {

class SyncCredentials;

// The engine's components for a Compass chain (docs/design/sync.md,
// "Plugging it into Chromium", route 2). Local sync runs the engine with no
// identity; this factory replaces the LoopbackConnectionManager that local
// sync would use with a network connection to the chain's server, which
// signs each request with the chain's key, and a scheduler that doesn't wait
// for OAuth credentials.
class CompassEngineComponentsFactory : public syncer::EngineComponentsFactory {
 public:
  CompassEngineComponentsFactory(
      const Switches& switches,
      const GURL& service_url,
      scoped_refptr<SyncCredentials> credentials,
      syncer::EngineComponentsFactoryImpl::HttpPostProviderFactoryGetter
          http_factory_getter);
  CompassEngineComponentsFactory(const CompassEngineComponentsFactory&) =
      delete;
  CompassEngineComponentsFactory& operator=(
      const CompassEngineComponentsFactory&) = delete;
  ~CompassEngineComponentsFactory() override;

  // <service_url path>/command/?client=...&client_id=<cache_guid>, as
  // EngineComponentsFactoryImpl builds it.
  static GURL MakeConnectionURL(const GURL& service_url,
                                const std::string& client_id);

  // syncer::EngineComponentsFactory:
  std::unique_ptr<syncer::SyncScheduler> BuildScheduler(
      const std::string& name,
      syncer::SyncCycleContext* context,
      syncer::CancelationSignal* cancelation_signal) override;
  std::unique_ptr<syncer::SyncCycleContext> BuildContext(
      syncer::ServerConnectionManager* connection_manager,
      syncer::ExtensionsActivity* extensions_activity,
      const std::vector<syncer::SyncEngineEventListener*>& listeners,
      syncer::DebugInfoGetter* debug_info_getter,
      syncer::DataTypeRegistry* data_type_registry,
      const std::string& cache_guid,
      const std::string& store_birthday,
      const std::string& bag_of_chips,
      base::TimeDelta poll_interval,
      const std::string& account_email,
      syncer::SyncAccessTokenFetcher* sync_access_token_fetcher) override;
  std::unique_ptr<syncer::ServerConnectionManager> BuildConnectionManager(
      const std::string& cache_guid,
      syncer::CancelationSignal* cancelation_signal) override;

 private:
  const Switches switches_;
  const GURL service_url_;
  scoped_refptr<SyncCredentials> credentials_;
  syncer::EngineComponentsFactoryImpl::HttpPostProviderFactoryGetter
      http_factory_getter_;
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_ENGINE_COMPONENTS_FACTORY_H_
