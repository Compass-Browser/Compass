// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_WINDOW_SYNC_WINDOW_FEATURES_H_
#define STRATA_BROWSER_WINDOW_SYNC_WINDOW_FEATURES_H_

#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/tab_strip_bridge.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;

namespace strata {

// Strata's per-window state. Owned by upstream's EmbedderBrowserWindowFeatures
// (see patches/) and created before the window's views.
//
// The SpaceModel is per window until window sync moves it to the space
// (ARCHITECTURE.md §7, docs/design/sidebar.md slice b).
class WindowFeatures {
 public:
  DECLARE_USER_DATA(WindowFeatures);

  explicit WindowFeatures(BrowserWindowInterface* browser);
  WindowFeatures(const WindowFeatures&) = delete;
  WindowFeatures& operator=(const WindowFeatures&) = delete;
  ~WindowFeatures();

  static WindowFeatures* From(BrowserWindowInterface* browser);

  SpaceModel& space_model() { return space_model_; }
  TabStripBridge& tab_strip_bridge() { return tab_strip_bridge_; }

 private:
  // Declared first so it outlives the bridge.
  SpaceModel space_model_;
  TabStripBridge tab_strip_bridge_;

  // Makes From() work. Upstream's EmbedderBrowserWindowFeatures::From() can't
  // be used: it never registers itself.
  ui::ScopedUnownedUserData<WindowFeatures> scoped_data_holder_;
};

}  // namespace strata

#endif  // STRATA_BROWSER_WINDOW_SYNC_WINDOW_FEATURES_H_
