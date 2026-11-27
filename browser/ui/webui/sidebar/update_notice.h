// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SIDEBAR_UPDATE_NOTICE_H_
#define COMPASS_BROWSER_UI_WEBUI_SIDEBAR_UPDATE_NOTICE_H_

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/scoped_observation.h"
#include "chrome/browser/upgrade_detector/build_state.h"
#include "chrome/browser/upgrade_detector/build_state_observer.h"
#include "components/prefs/pref_change_registrar.h"

namespace compass {

// Whether a window's sidebar shows the "update ready" card above the now
// playing widgets (docs/design/sidebar.md, "Update ready"): once an update is
// downloaded and waits for a relaunch (upstream's BuildState, which the
// updaters feed on macOS and Windows), unless the About page's switch
// (compass.update_sidebar_notice) is off or the card was closed for that
// update. Closing it hides it in every window until a newer update comes.
class UpdateNotice : public BuildStateObserver {
 public:
  // |on_changed| runs whenever ShouldShow() may have changed.
  explicit UpdateNotice(base::RepeatingClosure on_changed);
  UpdateNotice(const UpdateNotice&) = delete;
  UpdateNotice& operator=(const UpdateNotice&) = delete;
  ~UpdateNotice() override;

  bool ShouldShow() const;

  // Hides the card in every window until a newer update is ready.
  void Dismiss();

  // Relaunches into the update, as the app menu's "Relaunch to update".
  static void Relaunch();

  // BuildStateObserver:
  void OnUpdate(const BuildState* build_state) override;

 private:
  const base::RepeatingClosure on_changed_;
  PrefChangeRegistrar local_prefs_;
  base::ScopedObservation<BuildState, BuildStateObserver> build_state_{this};
  base::CallbackListSubscription dismissed_subscription_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SIDEBAR_UPDATE_NOTICE_H_
