// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/app_menu_sync.h"

#include "base/strings/strcat.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "compass/browser/sync/compass_sync_service.h"
#include "compass/browser/sync/compass_sync_service_factory.h"
#include "compass/browser/sync/sync_settings.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "compass/browser/ui/webui/sync/compass_sync_ui.h"
#include "content/public/common/url_constants.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

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
  NavigateParams params(browser,
                        GURL(base::StrCat({content::kChromeUIScheme, "://",
                                           kCompassSyncHost, "/"})),
                        ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  // A new Today entry, as the command bar's "Import from Arc".
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  params.window_action = NavigateParams::WindowAction::kShowWindow;
  Navigate(&params);
  return true;
}

}  // namespace compass
