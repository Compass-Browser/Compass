// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The desktop's LiveFolderService: a KeyedService per profile, on
// SpaceService's spaces. (Android's is SpacesAndroid's: spaces_android.cc.)

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/ui/live_folders/live_folder_service.h"
#include "compass/browser/ui/live_folders/live_folder_spaces.h"
#include "compass/browser/window_sync/live_tab_registry.h"
#include "compass/browser/window_sync/space_service.h"

namespace compass {

namespace {

// SpaceService's spaces, and its live tabs.
class DesktopLiveFolderSpaces : public LiveFolderSpaces {
 public:
  explicit DesktopLiveFolderSpaces(Profile* profile) : profile_(profile) {}
  ~DesktopLiveFolderSpaces() override = default;

  // LiveFolderSpaces:
  void RunWhenLoaded(base::OnceClosure callback) override {
    // Not while the profile is being made: SpaceService reads the sidebar.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<DesktopLiveFolderSpaces> self,
               base::OnceClosure callback) {
              if (!self) {
                return;
              }
              SpaceService* spaces = SpaceService::Get(self->profile_);
              self->service_ = spaces->GetWeakPtr();
              spaces->RunWhenLoaded(std::move(callback));
            },
            weak_factory_.GetWeakPtr(), std::move(callback)));
  }
  SpaceList* list() override {
    return service_ ? &service_->space_list() : nullptr;
  }
  bool HasTab(const SidebarNode& node) override {
    return service_ && service_->registry().GetTab(&node);
  }

 private:
  const raw_ptr<Profile> profile_;
  base::WeakPtr<SpaceService> service_;
  base::WeakPtrFactory<DesktopLiveFolderSpaces> weak_factory_{this};
};

}  // namespace

// static
LiveFolderService* LiveFolderService::Get(Profile* profile) {
  return LiveFolderServiceFactory::GetForProfile(profile);
}

// static
LiveFolderServiceFactory* LiveFolderServiceFactory::GetInstance() {
  static base::NoDestructor<LiveFolderServiceFactory> instance;
  return instance.get();
}

// static
LiveFolderService* LiveFolderServiceFactory::GetForProfile(Profile* profile) {
  return static_cast<LiveFolderService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

LiveFolderServiceFactory::LiveFolderServiceFactory()
    : ProfileKeyedServiceFactory(
          "CompassLiveFolderService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {}

LiveFolderServiceFactory::~LiveFolderServiceFactory() = default;

std::unique_ptr<KeyedService>
LiveFolderServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  return std::make_unique<LiveFolderService>(
      profile, std::make_unique<DesktopLiveFolderSpaces>(profile));
}

bool LiveFolderServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

bool LiveFolderServiceFactory::ServiceIsNULLWhileTesting() const {
  return true;
}

}  // namespace compass
