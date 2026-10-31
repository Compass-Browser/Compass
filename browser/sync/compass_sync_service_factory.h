// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_COMPASS_SYNC_SERVICE_FACTORY_H_
#define COMPASS_BROWSER_SYNC_COMPASS_SYNC_SERVICE_FACTORY_H_

#include <memory>

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class Profile;

namespace compass::sync {

class CompassSyncService;

// Built at startup with the other factories (see
// patches/chrome-browser-profiles-chrome_browser_main_extra_parts_profiles.cc.patch),
// which registers the compass.sync.* prefs and creates the service with
// each regular profile.
class CompassSyncServiceFactory : public ProfileKeyedServiceFactory {
 public:
  // Null for profiles without one (incognito, guest, tests).
  static CompassSyncService* GetForProfile(Profile* profile);
  static CompassSyncServiceFactory* GetInstance();

  CompassSyncServiceFactory(const CompassSyncServiceFactory&) = delete;
  CompassSyncServiceFactory& operator=(const CompassSyncServiceFactory&) =
      delete;

 private:
  friend base::NoDestructor<CompassSyncServiceFactory>;

  CompassSyncServiceFactory();
  ~CompassSyncServiceFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
  bool ServiceIsNULLWhileTesting() const override;
  void RegisterProfilePrefs(
      user_prefs::PrefRegistrySyncable* registry) override;
};

}  // namespace compass::sync

#endif  // COMPASS_BROWSER_SYNC_COMPASS_SYNC_SERVICE_FACTORY_H_
