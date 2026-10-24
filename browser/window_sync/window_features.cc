// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/window_features.h"

#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_collection.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/desktop_browser_window_capabilities.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "strata/browser/window_sync/sidebar_store.h"

namespace strata {

DEFINE_USER_DATA(WindowFeatures);

WindowFeatures::WindowFeatures(BrowserWindowInterface* browser)
    : service_(SpaceService::Get(browser->GetProfile())),
      tab_strip_bridge_(browser,
                        &service_->space_model(),
                        &service_->registry(),
                        &service_->restored_tab_matcher()),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {}

WindowFeatures::~WindowFeatures() {
  // Quitting closes every window but may never destroy the profile, so don't
  // leave pending changes to the store's destructor.
  if (SidebarStore* store = service_->store()) {
    store->Flush();
  }
}

// static
WindowFeatures* WindowFeatures::From(BrowserWindowInterface* browser) {
  return Get(browser->GetUnownedUserDataHost());
}

bool HandOffTabsBeforeClose(BrowserWindowInterface* browser) {
  if (browser_shutdown::IsTryingToQuit() ||
      browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      browser->GetTabStripModel()->empty()) {
    return false;
  }
  WindowFeatures* source = WindowFeatures::From(browser);
  if (!source) {
    return false;
  }
  // The window the user was in last: where they'd look for the tabs.
  WindowFeatures* target = nullptr;
  ProfileBrowserCollection::GetForProfile(browser->GetProfile())
      ->ForEach(
          [&](BrowserWindowInterface* other) {
            if (other == browser ||
                other->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
                other->capabilities()->IsAttemptingToCloseBrowser()) {
              return true;
            }
            target = WindowFeatures::From(other);
            return !target;
          },
          BrowserCollection::Order::kActivation);
  if (!target) {
    return false;
  }
  source->tab_strip_bridge().HandOffTabsTo(target->tab_strip_bridge());
  return true;
}

}  // namespace strata
