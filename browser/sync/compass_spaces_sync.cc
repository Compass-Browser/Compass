// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/compass_spaces_sync.h"

#include <memory>
#include <vector>

#include "base/auto_reset.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sync/data_type_store_service_factory.h"
#include "chrome/common/channel_info.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sync/compass_spaces_sync_bridge.h"
#include "compass/browser/sync/compass_sync_service.h"
#include "compass/browser/sync/compass_sync_service_factory.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"
#include "compass/components/sync/controllers_hook.h"
#include "components/sync/base/data_type.h"
#include "components/sync/base/report_unrecoverable_error.h"
#include "components/sync/model/client_tag_based_data_type_processor.h"
#include "components/sync/model/data_type_store_service.h"
#include "components/sync/model/proxy_data_type_controller_delegate.h"
#include "components/sync/service/data_type_controller.h"

namespace compass::sync {

namespace {

// The bridge, and what it needs from windows: deleting spaces and moving
// entries with their live tabs (window_features.h), and the switcher's order
// of the profile's spaces, which the bridge reads from (and writes to) the
// profile's SpaceList. Owned by the profile's SpaceService.
class SpacesSyncHolder : public SpaceService::Attachment,
                         public CompassSpacesSyncBridge::Delegate,
                         public SpaceList::Observer,
                         public SpaceDirectory::Observer {
 public:
  SpacesSyncHolder(Profile* profile, SpaceService* service)
      : profile_(profile), service_(service) {
    bridge_ = std::make_unique<CompassSpacesSyncBridge>(
        std::make_unique<syncer::ClientTagBasedDataTypeProcessor>(
            syncer::COMPASS_SPACES,
            base::BindRepeating(&syncer::ReportUnrecoverableError,
                                chrome::GetChannel())),
        DataTypeStoreServiceFactory::GetForProfile(profile)->GetStoreFactory(),
        &service->space_list(), this);
    service->space_list().AddObserver(this);
    if (SpaceDirectory* directory = SpaceDirectory::GetIfExists()) {
      directory->AddObserver(this);
    }
    service->RunWhenLoaded(base::BindOnce(&SpacesSyncHolder::OnLoaded,
                                          weak_factory_.GetWeakPtr()));
  }
  SpacesSyncHolder(const SpacesSyncHolder&) = delete;
  SpacesSyncHolder& operator=(const SpacesSyncHolder&) = delete;

  ~SpacesSyncHolder() override {
    // The directory goes with the profile manager, maybe first.
    if (SpaceDirectory* directory = SpaceDirectory::GetIfExists()) {
      directory->RemoveObserver(this);
    }
    service_->space_list().RemoveObserver(this);
    bridge_.reset();
  }

  CompassSpacesSyncBridge& bridge() { return *bridge_; }

  // CompassSpacesSyncBridge::Delegate:
  void DeleteSpace(SpaceModel& space) override {
    ::compass::DeleteSpace(profile_, space);
  }
  void MoveEntry(SidebarNode* node,
                 SpaceModel& target,
                 SidebarNode* parent,
                 size_t index) override {
    ::compass::MoveEntry(profile_, node, target, parent, index);
  }

  // SpaceList::Observer:
  void OnSpaceMoved(const SpaceModel& space) override {
    PushOrderToDirectory();
  }

  // SpaceDirectory::Observer:
  void OnDirectoryChanged() override { PullOrderFromDirectory(); }

 private:
  void OnLoaded() {
    PullOrderFromDirectory();
    bridge_->OnModelLoaded();
  }

  // The profile's spaces in the switcher's order; empty if the directory
  // doesn't list them (yet).
  std::vector<SpaceModel*> DirectoryOrder() const {
    std::vector<SpaceModel*> order;
    SpaceDirectory* directory = SpaceDirectory::GetIfExists();
    if (!directory || !directory->IsListed(profile_)) {
      return order;
    }
    for (const SpaceDirectory::Entry& entry : directory->GetSpaces()) {
      if (entry.profile == profile_) {
        order.push_back(entry.space);
      }
    }
    return order;
  }

  std::vector<SpaceModel*> ListOrder() const {
    std::vector<SpaceModel*> order;
    for (const auto& space : service_->space_list().spaces()) {
      order.push_back(space.get());
    }
    return order;
  }

  // The user reordered the switcher: the list follows, and sync sends it.
  void PullOrderFromDirectory() {
    if (syncing_order_) {
      return;
    }
    const std::vector<SpaceModel*> order = DirectoryOrder();
    if (order.size() != service_->space_list().spaces().size() ||
        order == ListOrder()) {
      return;
    }
    base::AutoReset<bool> syncing(&syncing_order_, true);
    SpaceList& list = service_->space_list();
    for (size_t i = 0; i < order.size(); ++i) {
      if (list.spaces()[i].get() != order[i]) {
        list.MoveSpace(*order[i], i);
      }
    }
  }

  // Sync reordered the list: the switcher follows.
  void PushOrderToDirectory() {
    if (syncing_order_) {
      return;
    }
    SpaceDirectory* directory = SpaceDirectory::GetIfExists();
    const std::vector<SpaceModel*> list = ListOrder();
    const std::vector<SpaceModel*> order = DirectoryOrder();
    if (!directory || order.size() != list.size() || order == list) {
      return;
    }
    base::AutoReset<bool> syncing(&syncing_order_, true);
    for (size_t i = 1; i < list.size(); ++i) {
      directory->PlaceAfter(*list[i], *list[i - 1]);
    }
  }

  const raw_ptr<Profile> profile_;
  const raw_ptr<SpaceService> service_;
  std::unique_ptr<CompassSpacesSyncBridge> bridge_;
  bool syncing_order_ = false;
  base::WeakPtrFactory<SpacesSyncHolder> weak_factory_{this};
};

// Runs COMPASS_SPACES only once the chain's passphrase is in place, so
// nothing is committed under a key the server knows (sync.md, "Slice 2
// notes", Passphrase).
class SpacesDataTypeController : public syncer::DataTypeController {
 public:
  SpacesDataTypeController(
      Profile* profile,
      std::unique_ptr<syncer::DataTypeControllerDelegate> full_sync,
      std::unique_ptr<syncer::DataTypeControllerDelegate> transport)
      : syncer::DataTypeController(syncer::COMPASS_SPACES,
                                   std::move(full_sync),
                                   std::move(transport)),
        profile_(profile) {}

  PreconditionState GetPreconditionState(
      const PreconditionContext& context) const override {
    CompassSyncService* service =
        CompassSyncServiceFactory::GetForProfileIfExists(profile_);
    return service && service->AreDataTypesReleased()
               ? PreconditionState::kPreconditionsMet
               : PreconditionState::kMustStopAndKeepData;
  }

 private:
  const raw_ptr<Profile> profile_;
};

// The bridge's processor, creating the bridge on first use.
base::WeakPtr<syncer::DataTypeControllerDelegate> GetControllerDelegate(
    Profile* profile) {
  SpaceService* service = SpaceService::Get(profile);
  // This is the only kind of attachment.
  auto* holder = static_cast<SpacesSyncHolder*>(service->sync_attachment());
  if (!holder) {
    auto owned = std::make_unique<SpacesSyncHolder>(profile, service);
    holder = owned.get();
    service->SetSyncAttachment(std::move(owned));
  }
  return holder->bridge().change_processor()->GetControllerDelegate();
}

syncer::DataTypeController::TypeVector CreateControllers(
    content::BrowserContext* context,
    syncer::SyncService* sync_service) {
  Profile* profile = Profile::FromBrowserContext(context);
  // The sync service, and its controllers, go before the profile.
  const syncer::ProxyDataTypeControllerDelegate::DelegateProvider provider =
      base::BindRepeating(&GetControllerDelegate, base::Unretained(profile));
  const scoped_refptr<base::SequencedTaskRunner> runner =
      base::SequencedTaskRunner::GetCurrentDefault();
  syncer::DataTypeController::TypeVector controllers;
  // Local sync runs the types in full-sync mode; transport mode gets the
  // same bridge in case.
  controllers.push_back(std::make_unique<SpacesDataTypeController>(
      profile,
      std::make_unique<syncer::ProxyDataTypeControllerDelegate>(runner,
                                                                provider),
      std::make_unique<syncer::ProxyDataTypeControllerDelegate>(runner,
                                                                provider)));
  return controllers;
}

}  // namespace

void RegisterSpacesSyncController() {
  SetExtraControllersFactory(base::BindRepeating(&CreateControllers));
}

}  // namespace compass::sync
