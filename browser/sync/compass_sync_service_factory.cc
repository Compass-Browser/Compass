// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/compass_sync_service_factory.h"

#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sync/device_info_sync_service_factory.h"
#include "chrome/browser/sync/sync_service_factory.h"
#include "build/build_config.h"
#if !BUILDFLAG(IS_ANDROID)
#include "chrome/browser/ui/browser_manager_service_factory.h"
#endif
#include "compass/browser/sync/compass_spaces_sync.h"
#include "compass/browser/sync/compass_sync_service.h"

namespace compass::sync {

// static
CompassSyncService* CompassSyncServiceFactory::GetForProfile(Profile* profile) {
  return static_cast<CompassSyncService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
CompassSyncService* CompassSyncServiceFactory::GetForProfileIfExists(
    Profile* profile) {
  return static_cast<CompassSyncService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/false));
}

CompassSyncServiceFactory* CompassSyncServiceFactory::GetInstance() {
  static base::NoDestructor<CompassSyncServiceFactory> instance;
  return instance.get();
}

CompassSyncServiceFactory::CompassSyncServiceFactory()
    : ProfileKeyedServiceFactory("CompassSyncService") {
  DependsOn(SyncServiceFactory::GetInstance());
  // The devices list on chrome://compass-sync.
  DependsOn(DeviceInfoSyncServiceFactory::GetInstance());
  // ProfileBrowserCollection, for the refresh on focus.
#if !BUILDFLAG(IS_ANDROID)
  DependsOn(BrowserManagerServiceFactory::GetInstance());
#endif
  // Built before any profile loads: from now on, sync services get the
  // COMPASS_SPACES controller.
  RegisterSpacesSyncController();
}

CompassSyncServiceFactory::~CompassSyncServiceFactory() = default;

std::unique_ptr<KeyedService>
CompassSyncServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  return std::make_unique<CompassSyncService>(
      profile, SyncServiceFactory::GetForProfile(profile),
      g_browser_process->os_crypt_async());
}

bool CompassSyncServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  // The chain's key and passphrase must be in place when sync starts.
  return true;
}

bool CompassSyncServiceFactory::ServiceIsNULLWhileTesting() const {
  return true;
}

void CompassSyncServiceFactory::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  CompassSyncService::RegisterProfilePrefs(registry);
}

}  // namespace compass::sync
