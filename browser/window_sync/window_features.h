// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_WINDOW_SYNC_WINDOW_FEATURES_H_
#define STRATA_BROWSER_WINDOW_SYNC_WINDOW_FEATURES_H_

#include <memory>

#include "base/callback_list.h"
#include "base/check.h"
#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/space_service.h"
#include "strata/browser/window_sync/tab_strip_bridge.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;

namespace strata {

// Strata's per-window state: the space the window shows, and the bridge
// between the window's tab strip and that space. Owned by upstream's
// EmbedderBrowserWindowFeatures (see patches/) and created before the window's
// views. The profile's parking window gets these too, without a bridge: its
// tabs keep the nodes they had. See docs/design/spaces.md.
class WindowFeatures {
 public:
  DECLARE_USER_DATA(WindowFeatures);

  class Observer : public base::CheckedObserver {
   public:
    // The window now shows another space, with a new bridge.
    virtual void OnSpaceSwitched() {}
  };

  explicit WindowFeatures(BrowserWindowInterface* browser);
  WindowFeatures(const WindowFeatures&) = delete;
  WindowFeatures& operator=(const WindowFeatures&) = delete;
  ~WindowFeatures();

  static WindowFeatures* From(BrowserWindowInterface* browser);

  SpaceService& service() { return *service_; }
  // The space this window shows. Shared with the profile's other windows.
  SpaceModel& space_model() { return *space_; }
  LiveTabRegistry& registry() { return service_->registry(); }

  bool is_parking_window() const { return is_parking_window_; }
  // Set during SwitchToSpace(), while the window has no bridge for a moment.
  bool is_switching() const { return switching_; }
  // Not for the parking window.
  TabStripBridge& tab_strip_bridge() {
    CHECK(bridge_);
    return *bridge_;
  }

  // Shows |space| in this window. The live tabs of the space it showed go to
  // another window showing that space, or to the parking window; those of
  // |space| come back from the parking window. See docs/design/spaces.md,
  // "Switching, step by step".
  void SwitchToSpace(SpaceModel& space);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

 private:
  void OnActiveTabChanged(BrowserWindowInterface* browser);
  void OnWindowBecameActive(BrowserWindowInterface* browser);

  // Another normal window of the profile that shows |space|, or null.
  BrowserWindowInterface* FindOtherWindowShowing(const SpaceModel& space);

  const raw_ptr<BrowserWindowInterface> browser_;
  // Outlives the window: it belongs to the profile.
  const raw_ptr<SpaceService> service_;
  raw_ptr<SpaceModel> space_;
  const bool is_parking_window_;
  bool switching_ = false;
  // Null for the parking window.
  std::unique_ptr<TabStripBridge> bridge_;

  base::ObserverList<Observer> observers_;
  base::CallbackListSubscription active_tab_subscription_;
  base::CallbackListSubscription did_become_active_subscription_;

  // Makes From() work. Upstream's EmbedderBrowserWindowFeatures::From() can't
  // be used: it never registers itself.
  ui::ScopedUnownedUserData<WindowFeatures> scoped_data_holder_;
};

// Called when |browser|'s window is asked to close, before beforeunload
// handlers run. Hands its tabs to the most recently active other window of
// the profile that shows the same space, unless the app is quitting. Returns
// true if it did; the emptied window then closes on its own, and the caller
// must not go on closing it. See docs/design/sidebar.md, "Window sync".
bool HandOffTabsBeforeClose(BrowserWindowInterface* browser);

}  // namespace strata

#endif  // STRATA_BROWSER_WINDOW_SYNC_WINDOW_FEATURES_H_
