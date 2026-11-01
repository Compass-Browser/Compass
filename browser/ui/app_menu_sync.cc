// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/app_menu_sync.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "compass/browser/sync/compass_sync_service.h"
#include "compass/browser/sync/compass_sync_service_factory.h"
#include "compass/browser/sync/sync_settings.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "compass/browser/ui/webui/settings/settings_sections.h"

namespace compass {

std::optional<std::u16string> GetAppMenuSyncLabel(
    BrowserWindowInterface* browser) {
  Profile* profile = browser ? browser->GetProfile() : nullptr;
  if (!profile || profile->IsOffTheRecord() || profile->IsGuestSession()) {
    return std::nullopt;
  }
  sync::CompassSyncService* service =
      sync::CompassSyncServiceFactory::GetForProfile(profile);
  if (!service) {
    return std::nullopt;
  }
  return sync::GetSyncMenuLabel(service->GetStatus());
}

bool HandleAppMenuSyncCommand(BrowserWindowInterface* browser, int id) {
  if (id != kOpenSyncPageCommand) {
    return false;
  }
  chrome::ShowSettingsSubPage(browser, kSyncSettingsSubPage);
  return true;
}

}  // namespace compass
