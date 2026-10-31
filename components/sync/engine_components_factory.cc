// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/engine_components_factory.h"

#include <optional>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/threading/platform_thread.h"
#include "compass/components/sync/sync_credentials.h"
#include "components/signin/public/identity_manager/access_token_info.h"
#include "components/sync/engine/backoff_delay_provider.h"
#include "components/sync/engine/cancelation_signal.h"
#include "components/sync/engine/cycle/sync_cycle_context.h"
#include "components/sync/engine/net/http_post_provider_factory.h"
#include "components/sync/engine/net/server_connection_manager.h"
#include "components/sync/engine/net/sync_server_connection_manager.h"
#include "components/sync/engine/net/url_translator.h"
#include "components/sync/engine/sync_scheduler_impl.h"
#include "components/sync/engine/syncer.h"

namespace compass::sync {
namespace {

// At startup the seed may still be decrypting when the engine sends its
// first request; it waits this long for the key, in steps of kKeyPollStep.
constexpr base::TimeDelta kKeyWait = base::Seconds(20);
constexpr base::TimeDelta kKeyPollStep = base::Milliseconds(100);

// Posts like SyncServerConnectionManager, but ignores the engine's (absent)
// OAuth token and signs each request with a fresh token from the chain key.
class CompassConnectionManager : public syncer::SyncServerConnectionManager {
 public:
  CompassConnectionManager(
      const GURL& sync_request_url,
      std::unique_ptr<syncer::HttpPostProviderFactory> factory,
      syncer::CancelationSignal* cancelation_signal,
      scoped_refptr<SyncCredentials> credentials)
      : syncer::SyncServerConnectionManager(sync_request_url,
                                            std::move(factory),
                                            cancelation_signal),
        sync_request_url_(sync_request_url),
        cancelation_signal_(cancelation_signal),
        credentials_(std::move(credentials)) {}

 protected:
  syncer::HttpResponse PostBuffer(
      const std::string& buffer_in,
      std::string* buffer_out,
      const signin::AccessTokenInfo& /*ignored*/) override {
    std::optional<std::string> token = WaitForToken();
    if (!token) {
      VLOG(1) << "Compass sync: no chain key, request not sent";
      return syncer::HttpResponse::ForUnspecifiedError();
    }
    const base::Time now = base::Time::Now();
    syncer::HttpResponse response =
        syncer::SyncServerConnectionManager::PostBuffer(
            buffer_in, buffer_out,
            signin::AccessTokenInfo(*token, now + base::Hours(1),
                                    /*id_token=*/std::string()));
    // For the end-to-end steps (docs/design/sync-slice-2-steps.md).
    VLOG(1) << "[compass-sync] POST " << sync_request_url_.GetPath() << " ("
            << buffer_in.size() << " bytes) -> " << response;
    return response;
  }

 private:
  // Runs on the sync sequence, which may block. Sleeps rather than waits on
  // an event: the sync sequence may not wait on //base sync primitives.
  std::optional<std::string> WaitForToken() {
    for (base::TimeDelta waited; waited < kKeyWait; waited += kKeyPollStep) {
      if (std::optional<std::string> token =
              credentials_->MakeToken(base::Time::Now())) {
        return token;
      }
      if (cancelation_signal_->IsSignalled()) {
        return std::nullopt;
      }
      base::PlatformThread::Sleep(kKeyPollStep);
    }
    return credentials_->MakeToken(base::Time::Now());
  }

  const GURL sync_request_url_;
  const raw_ptr<syncer::CancelationSignal> cancelation_signal_;
  const scoped_refptr<SyncCredentials> credentials_;
};

}  // namespace

CompassEngineComponentsFactory::CompassEngineComponentsFactory(
    const Switches& switches,
    const GURL& service_url,
    scoped_refptr<SyncCredentials> credentials,
    syncer::EngineComponentsFactoryImpl::HttpPostProviderFactoryGetter
        http_factory_getter)
    : switches_(switches),
      service_url_(service_url),
      credentials_(std::move(credentials)),
      http_factory_getter_(std::move(http_factory_getter)) {
  CHECK(credentials_);
  CHECK(http_factory_getter_);
}

CompassEngineComponentsFactory::~CompassEngineComponentsFactory() = default;

// static
GURL CompassEngineComponentsFactory::MakeConnectionURL(
    const GURL& service_url,
    const std::string& client_id) {
  std::string path = service_url.GetPath();
  if (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  path += "/command/";
  GURL::Replacements replacements;
  replacements.SetPathStr(path);
  return syncer::AppendSyncQueryString(
      service_url.ReplaceComponents(replacements), client_id);
}

std::unique_ptr<syncer::SyncScheduler>
CompassEngineComponentsFactory::BuildScheduler(
    const std::string& name,
    syncer::SyncCycleContext* context,
    syncer::CancelationSignal* cancelation_signal) {
  std::unique_ptr<syncer::BackoffDelayProvider> delay =
      (switches_.backoff_override == BACKOFF_SHORT_INITIAL_RETRY_OVERRIDE)
          ? syncer::BackoffDelayProvider::WithShortInitialRetryOverride()
          : syncer::BackoffDelayProvider::FromDefaults();
  auto scheduler = std::make_unique<syncer::SyncSchedulerImpl>(
      name, std::move(delay), context,
      std::make_unique<syncer::Syncer>(cancelation_signal),
      /*ignore_auth_credentials=*/true);
  if (switches_.force_short_nudge_delay_for_test) {
    scheduler->ForceShortNudgeDelayForTest();
  }
  return scheduler;
}

std::unique_ptr<syncer::SyncCycleContext>
CompassEngineComponentsFactory::BuildContext(
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
    syncer::SyncAccessTokenFetcher* sync_access_token_fetcher) {
  return std::make_unique<syncer::SyncCycleContext>(
      connection_manager, extensions_activity, listeners, debug_info_getter,
      data_type_registry, cache_guid, store_birthday, bag_of_chips,
      poll_interval, account_email, sync_access_token_fetcher);
}

std::unique_ptr<syncer::ServerConnectionManager>
CompassEngineComponentsFactory::BuildConnectionManager(
    const std::string& cache_guid,
    syncer::CancelationSignal* cancelation_signal) {
  CHECK(http_factory_getter_);
  return std::make_unique<CompassConnectionManager>(
      MakeConnectionURL(service_url_, cache_guid),
      std::move(http_factory_getter_).Run(), cancelation_signal, credentials_);
}

}  // namespace compass::sync
