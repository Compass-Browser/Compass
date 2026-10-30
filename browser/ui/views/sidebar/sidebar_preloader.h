// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_PRELOADER_H_
#define STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_PRELOADER_H_

#include <memory>

#include "base/containers/flat_map.h"
#include "base/no_destructor.h"
#include "base/scoped_multi_source_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/profiles/profile_observer.h"

class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace strata {

// Keeps a hidden chrome://strata-sidebar loaded for each listed profile that
// an open window of another profile could switch to, so a switch to another
// profile's space doesn't wait for a renderer to launch: the new window's
// sidebar takes it (StrataSidebarView). Its page connects with no window, and
// gets one when taken (StrataSidebarUI). As upstream's
// WebUIContentsPreloadManager does for tab search. See
// docs/design/profiles-in-place.md, "Preloaded sidebars".
class SidebarPreloader : public ProfileObserver {
 public:
  static SidebarPreloader* Get();

  SidebarPreloader(const SidebarPreloader&) = delete;
  SidebarPreloader& operator=(const SidebarPreloader&) = delete;

  // |profile|'s preloaded sidebar, or null. The next one loads with the next
  // refill.
  std::unique_ptr<content::WebContents> Take(Profile* profile);

  // Refills after kRefillDelay, so it doesn't compete with what is opening
  // now. Called when a window opens, and when a switch has finished.
  void RefillLater();
  // Preloads for every profile that needs it and has none: each listed
  // profile with an open window of another profile.
  void Refill();

  // The preloaded sidebar of |profile|, if any.
  content::WebContents* GetForTesting(Profile* profile);
  void SetRefillDelayForTesting(base::TimeDelta delay);

  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override;

 private:
  friend class base::NoDestructor<SidebarPreloader>;

  SidebarPreloader();
  ~SidebarPreloader() override;

  void Preload(Profile* profile);

  base::flat_map<Profile*, std::unique_ptr<content::WebContents>> preloaded_;
  base::ScopedMultiSourceObservation<Profile, ProfileObserver>
      profile_observations_{this};
  base::TimeDelta refill_delay_;
  base::OneShotTimer refill_timer_;
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_PRELOADER_H_
