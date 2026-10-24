// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_WINDOW_SYNC_WINDOW_FEATURES_H_
#define STRATA_BROWSER_WINDOW_SYNC_WINDOW_FEATURES_H_

#include "base/memory/raw_ptr.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/space_service.h"
#include "strata/browser/window_sync/tab_strip_bridge.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;

namespace strata {

// Strata's per-window state: the bridge between this window's tab strip and
// its profile's SpaceService. Owned by upstream's
// EmbedderBrowserWindowFeatures (see patches/) and created before the window's
// views.
class WindowFeatures {
 public:
  DECLARE_USER_DATA(WindowFeatures);

  explicit WindowFeatures(BrowserWindowInterface* browser);
  WindowFeatures(const WindowFeatures&) = delete;
  WindowFeatures& operator=(const WindowFeatures&) = delete;
  ~WindowFeatures();

  static WindowFeatures* From(BrowserWindowInterface* browser);

  // Shared with the profile's other windows.
  SpaceModel& space_model() { return service_->space_model(); }
  LiveTabRegistry& registry() { return service_->registry(); }

  TabStripBridge& tab_strip_bridge() { return tab_strip_bridge_; }

 private:
  // Outlives the window: it belongs to the profile.
  const raw_ptr<SpaceService> service_;
  TabStripBridge tab_strip_bridge_;

  // Makes From() work. Upstream's EmbedderBrowserWindowFeatures::From() can't
  // be used: it never registers itself.
  ui::ScopedUnownedUserData<WindowFeatures> scoped_data_holder_;
};

}  // namespace strata

#endif  // STRATA_BROWSER_WINDOW_SYNC_WINDOW_FEATURES_H_
