// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/window_features.h"

#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"

namespace strata {

DEFINE_USER_DATA(WindowFeatures);

WindowFeatures::WindowFeatures(BrowserWindowInterface* browser)
    : tab_strip_bridge_(browser->GetTabStripModel(), &space_model_),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {}

WindowFeatures::~WindowFeatures() = default;

// static
WindowFeatures* WindowFeatures::From(BrowserWindowInterface* browser) {
  return Get(browser->GetUnownedUserDataHost());
}

}  // namespace strata
