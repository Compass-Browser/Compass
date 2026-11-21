// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_DEV_MODE_DEV_MODE_CONTROLLER_H_
#define COMPASS_BROWSER_UI_VIEWS_DEV_MODE_DEV_MODE_CONTROLLER_H_

#include <memory>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/prefs/pref_change_registrar.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;

namespace tabs {
class TabInterface;
}  // namespace tabs

namespace compass {

class ActiveTabWatcher;

// A window's development mode (docs/design/dev-mode.md): each pane whose
// page is on a site in development mode gets a DevModeBar at its top, and
// loses it when the page leaves the site or the site leaves the list.
// Owned by the window's sidebar view, as ReaderMode.
class DevModeController {
 public:
  DECLARE_USER_DATA(DevModeController);

  explicit DevModeController(BrowserWindowInterface* browser);
  DevModeController(const DevModeController&) = delete;
  DevModeController& operator=(const DevModeController&) = delete;
  ~DevModeController();

  static DevModeController* From(BrowserWindowInterface* browser);

  // Whether the active tab's page is in development mode.
  bool IsEnabledForActiveTab() const;
  // Whether the active tab's page can be (a web page).
  bool CanToggleForActiveTab() const;
  // Turns development mode on for the active tab's site, or off.
  void ToggleForActiveTab();

  // Adds and removes the panes' bars. Public for tests.
  void Sync();

 private:
  class PageWatcher;

  void OnActiveTabChanged(const tabs::TabInterface* tab);
  void SyncSoon();

  const raw_ptr<BrowserWindowInterface> browser_;
  PrefChangeRegistrar pref_change_registrar_;
  std::unique_ptr<ActiveTabWatcher> active_tab_watcher_;
  std::vector<base::CallbackListSubscription> pane_subscriptions_;
  // The panes' pages: one that commits a new page may come in or out of
  // development mode.
  std::vector<std::unique_ptr<PageWatcher>> page_watchers_;
  ui::ScopedUnownedUserData<DevModeController> scoped_data_holder_;
  base::WeakPtrFactory<DevModeController> weak_factory_{this};
};

// The browser command kToggleDevModeCommand. Returns true if it handled
// |id|. Called from compass::HandleBrowserCommand().
bool HandleDevModeCommand(BrowserWindowInterface* browser, int id);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_DEV_MODE_DEV_MODE_CONTROLLER_H_
