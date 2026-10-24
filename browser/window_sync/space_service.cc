// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/space_service.h"

#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/ptr_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "strata/browser/window_sync/sidebar_store.h"
#include "strata/browser/window_sync/window_features.h"

namespace strata {

namespace {

const char kUserDataKey[] = "strata_space_service";

constexpr base::FilePath::CharType kStoreFileName[] =
    FILE_PATH_LITERAL("Strata Sidebar.json");

// The first auto-archive pass waits for session restore to hand its tabs
// their saved entries.
constexpr base::TimeDelta kFirstArchiveDelay = base::Seconds(30);
// Delays are hours or more, so this is precise enough.
constexpr base::TimeDelta kArchiveInterval = base::Minutes(10);

BrowserWindowInterface* CreateParkingWindow(Profile* profile) {
  // A popup, so upstream never picks it for new tabs or links from other
  // apps, and never restored as a window.
  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_POPUP, profile,
                                   /*from_user_gesture=*/false);
  params.omit_from_session_restore = true;
  return CreateBrowserWindow(std::move(params));
}

}  // namespace

SpaceService::SpaceService(Profile* profile)
    : profile_(profile),
      parking_window_factory_(base::BindRepeating(&CreateParkingWindow)) {
  space_list_.AddObserver(this);
  for (const auto& space : space_list_.spaces()) {
    OnSpaceAdded(*space);
  }
  if (!profile->IsOffTheRecord()) {
    store_ = std::make_unique<SidebarStore>(
        profile->GetPath().Append(kStoreFileName), &space_list_);
    // The service lives as long as the profile, and so does its store.
    store_->Load(
        base::BindOnce(&SpaceService::OnLoaded, base::Unretained(this)));
  } else {
    OnLoaded();
  }
}

SpaceService::~SpaceService() {
  // The store observes the list, so it goes first.
  store_.reset();
  space_list_.RemoveObserver(this);
}

// static
SpaceService* SpaceService::Get(Profile* profile) {
  auto* service =
      static_cast<SpaceService*>(profile->GetUserData(kUserDataKey));
  if (!service) {
    service = new SpaceService(profile);
    profile->SetUserData(kUserDataKey, base::WrapUnique(service));
  }
  return service;
}

RestoredTabMatcher& SpaceService::restored_tab_matcher(
    const SpaceModel& space) {
  auto it = matchers_.find(&space);
  CHECK(it != matchers_.end());
  return *it->second;
}

SidebarNode* SpaceService::GetLastActiveNode(SpaceModel& space) {
  auto it = last_active_nodes_.find(&space);
  return it == last_active_nodes_.end() ? nullptr
                                        : space.FindById(it->second);
}

void SpaceService::SetLastActiveNode(const SpaceModel& space,
                                     const SidebarNode& node) {
  last_active_nodes_[&space] = node.id();
}

BrowserWindowInterface* SpaceService::GetOrCreateParkingWindow() {
  if (!parking_window()) {
    base::AutoReset<bool> creating(&creating_parking_window_, true);
    parking_window_ = parking_window_factory_.Run(profile_);
    CHECK(parking_window_);
  }
  return parking_window_;
}

BrowserWindowInterface* SpaceService::parking_window() const {
  return parking_window_ && !parking_window_->IsDeleteScheduled()
             ? parking_window_.get()
             : nullptr;
}

bool SpaceService::IsParkingWindow(const BrowserWindowInterface* browser) const {
  return browser && browser == parking_window_;
}

void SpaceService::OnParkingWindowDestroyed(
    const BrowserWindowInterface* browser) {
  // A newer one may have replaced it already.
  if (parking_window_ == browser) {
    parking_window_ = nullptr;
  }
}

void SpaceService::CloseParkingWindow() {
  if (BrowserWindowInterface* parking = parking_window()) {
    // Once empty, the window closes on its own.
    parking->GetTabStripModel()->CloseAllTabs();
  }
}

void SpaceService::SetParkingWindowFactoryForTesting(
    ParkingWindowFactory factory) {
  parking_window_factory_ = std::move(factory);
}

void SpaceService::ArchiveIdleEntries() {
  ArchiveIdleTodayEntries(profile_, base::Time::Now());
}

void SpaceService::OnSpaceAdded(SpaceModel& space) {
  auto matcher = std::make_unique<RestoredTabMatcher>(
      &space, &space_list_.favorites(), &registry_);
  if (loaded_) {
    matcher->OnLoaded();
  }
  matchers_[&space] = std::move(matcher);
}

void SpaceService::OnSpaceWillBeRemoved(SpaceModel& space) {
  matchers_.erase(&space);
  last_active_nodes_.erase(&space);
}

void SpaceService::OnLoaded() {
  loaded_ = true;
  for (auto& [space, matcher] : matchers_) {
    matcher->OnLoaded();
  }
  first_archive_timer_.Start(FROM_HERE, kFirstArchiveDelay, this,
                             &SpaceService::ArchiveIdleEntries);
  archive_timer_.Start(FROM_HERE, kArchiveInterval, this,
                       &SpaceService::ArchiveIdleEntries);
}

}  // namespace strata
