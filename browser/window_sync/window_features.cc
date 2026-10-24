// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/window_features.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "strata/browser/window_sync/sidebar_store.h"

namespace strata {

DEFINE_USER_DATA(WindowFeatures);

WindowFeatures::WindowFeatures(BrowserWindowInterface* browser)
    : service_(SpaceService::Get(browser->GetProfile())),
      tab_strip_bridge_(browser,
                        &service_->space_model(),
                        &service_->registry()),
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

}  // namespace strata
