// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/sidebar_preloader.h"

#include <utility>

#include "base/containers/flat_set.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_collection.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "strata/browser/ui/webui/sidebar/strata_sidebar_ui.h"
#include "strata/browser/window_sync/space_directory.h"
#include "strata/browser/window_sync/window_features.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/page_transition_types.h"
#include "ui/views/controls/webview/web_contents_set_background_color.h"
#include "url/gurl.h"

namespace strata {

namespace {

// Long enough for a window that just opened, or a switch that just finished,
// to have painted.
constexpr base::TimeDelta kRefillDelay = base::Seconds(2);

}  // namespace

// static
SidebarPreloader* SidebarPreloader::Get() {
  static base::NoDestructor<SidebarPreloader> preloader;
  return preloader.get();
}

SidebarPreloader::SidebarPreloader() : refill_delay_(kRefillDelay) {}

SidebarPreloader::~SidebarPreloader() = default;

std::unique_ptr<content::WebContents> SidebarPreloader::Take(Profile* profile) {
  auto it = preloaded_.find(profile);
  if (it == preloaded_.end()) {
    return nullptr;
  }
  std::unique_ptr<content::WebContents> contents = std::move(it->second);
  preloaded_.erase(it);
  // A renderer that went away would leave the new window's sidebar blank.
  if (contents->IsCrashed() ||
      contents->GetVisibleURL() != GURL(kStrataSidebarURL)) {
    return nullptr;
  }
  return contents;
}

void SidebarPreloader::RefillLater() {
  refill_timer_.Start(FROM_HERE, refill_delay_,
                      base::BindOnce(&SidebarPreloader::Refill,
                                     base::Unretained(this)));
}

void SidebarPreloader::Refill() {
  refill_timer_.Stop();
  SpaceDirectory* directory = SpaceDirectory::GetIfExists();
  if (!directory || browser_shutdown::IsTryingToQuit()) {
    return;
  }
  // The profiles a window could switch from.
  base::flat_set<Profile*> window_profiles;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* browser) {
        WindowFeatures* features = WindowFeatures::From(browser);
        if (browser->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
            features && !features->is_parking_window() &&
            !features->swapped_away() &&
            SpaceDirectory::IsListable(browser->GetProfile())) {
          window_profiles.insert(browser->GetProfile());
        }
        return true;
      },
      BrowserCollection::Order::kActivation);
  for (const SpaceDirectory::Entry& entry : directory->GetSpaces()) {
    Profile* profile = entry.profile;
    if (preloaded_.contains(profile) || profile->ShutdownStarted()) {
      continue;
    }
    const bool switchable =
        window_profiles.size() > 1 ||
        (window_profiles.size() == 1 && !window_profiles.contains(profile));
    if (switchable) {
      Preload(profile);
    }
  }
}

content::WebContents* SidebarPreloader::GetForTesting(Profile* profile) {
  auto it = preloaded_.find(profile);
  return it == preloaded_.end() ? nullptr : it->second.get();
}

void SidebarPreloader::SetRefillDelayForTesting(base::TimeDelta delay) {
  refill_delay_ = delay;
}

void SidebarPreloader::OnProfileWillBeDestroyed(Profile* profile) {
  profile_observations_.RemoveObservation(profile);
  preloaded_.erase(profile);
}

void SidebarPreloader::Preload(Profile* profile) {
  const GURL url(kStrataSidebarURL);
  content::WebContents::CreateParams params(profile);
  params.initially_hidden = true;
  params.site_instance = content::SiteInstance::CreateForURL(profile, url);
  std::unique_ptr<content::WebContents> contents =
      content::WebContents::Create(params);
  // As StrataSidebarView's own: the region's background shows through.
  views::WebContentsSetBackgroundColor::CreateForWebContentsWithColor(
      contents.get(), SK_ColorTRANSPARENT);
  contents->GetController().LoadURL(url, content::Referrer(),
                                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                    std::string());
  if (!profile_observations_.IsObservingSource(profile)) {
    profile_observations_.AddObservation(profile);
  }
  preloaded_[profile] = std::move(contents);
}

}  // namespace strata
