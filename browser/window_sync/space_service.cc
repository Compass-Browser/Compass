// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/space_service.h"

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/ptr_util.h"
#include "chrome/browser/profiles/profile.h"
#include "strata/browser/window_sync/sidebar_store.h"

namespace strata {

namespace {

const char kUserDataKey[] = "strata_space_service";

constexpr base::FilePath::CharType kStoreFileName[] =
    FILE_PATH_LITERAL("Strata Sidebar.json");

}  // namespace

SpaceService::SpaceService(Profile* profile) {
  if (!profile->IsOffTheRecord()) {
    store_ = std::make_unique<SidebarStore>(
        profile->GetPath().Append(kStoreFileName), &space_model_);
    // The service lives as long as the profile, and so does its store.
    store_->Load(base::BindOnce(&RestoredTabMatcher::OnLoaded,
                                base::Unretained(&restored_tab_matcher_)));
  } else {
    restored_tab_matcher_.OnLoaded();
  }
}

SpaceService::~SpaceService() = default;

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

}  // namespace strata
