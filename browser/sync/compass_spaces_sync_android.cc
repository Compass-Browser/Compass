// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// COMPASS_SPACES on Android (docs/design/android.md): the same bridge as on
// the desktop (compass_spaces_sync.cc), on SpacesAndroid's list. Android has
// one window and no space directory: the delegate moves entries in the model,
// and an entry is in use while it has a tab.

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sync/data_type_store_service_factory.h"
#include "chrome/common/channel_info.h"
#include "compass/browser/android/spaces_android.h"
#include "compass/browser/claude/claude_memory.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/sync/compass_spaces_sync.h"
#include "compass/browser/sync/compass_spaces_sync_bridge.h"
#include "compass/browser/sync/compass_sync_service.h"
#include "compass/browser/sync/compass_sync_service_factory.h"
#include "compass/components/sync/controllers_hook.h"
#include "components/sync/base/data_type.h"
#include "components/sync/base/report_unrecoverable_error.h"
#include "components/sync/model/client_tag_based_data_type_processor.h"
#include "components/sync/model/data_type_store_service.h"
#include "components/sync/model/proxy_data_type_controller_delegate.h"
#include "components/sync/service/data_type_controller.h"

namespace compass::sync {

namespace {

class SpacesSyncHolder : public SpacesAndroid::Attachment,
                         public CompassSpacesSyncBridge::Delegate,
                         public claude::ClaudeMemory::Observer {
 public:
  SpacesSyncHolder(Profile* profile, SpacesAndroid* spaces)
      : spaces_(spaces) {
    bridge_ = std::make_unique<CompassSpacesSyncBridge>(
        std::make_unique<syncer::ClientTagBasedDataTypeProcessor>(
            syncer::COMPASS_SPACES,
            base::BindRepeating(&syncer::ReportUnrecoverableError,
                                chrome::GetChannel())),
        DataTypeStoreServiceFactory::GetForProfile(profile)->GetStoreFactory(),
        &spaces->list(), this);
    // Claude's memory syncs with the spaces, as on the desktop
    // (compass_spaces_sync.cc; docs/design/claude.md, "Memory").
    if (claude::ClaudeMemory* memory = claude::ClaudeMemory::Get(profile)) {
      memory_ = memory->GetWeakPtr();
      memory->AddObserver(this);
    }
  }
  SpacesSyncHolder(const SpacesSyncHolder&) = delete;
  SpacesSyncHolder& operator=(const SpacesSyncHolder&) = delete;
  ~SpacesSyncHolder() override {
    if (memory_) {
      memory_->RemoveObserver(this);
    }
    bridge_.reset();
  }

  CompassSpacesSyncBridge& bridge() { return *bridge_; }

  // CompassSpacesSyncBridge::Delegate:
  std::optional<std::vector<sync_pb::CompassMemory>> GetLocalMemories()
      override {
    if (!memory_ || !memory_->loaded()) {
      return std::nullopt;
    }
    std::vector<sync_pb::CompassMemory> memories;
    for (const claude::MemoryEntry& entry : memory_->entries()) {
      sync_pb::CompassMemory& memory = memories.emplace_back();
      memory.set_uuid(entry.id);
      memory.set_text(entry.text);
      memory.set_create_time(
          entry.created.ToDeltaSinceWindowsEpoch().InMicroseconds());
      memory.set_update_time(
          entry.updated.ToDeltaSinceWindowsEpoch().InMicroseconds());
    }
    return memories;
  }

  void ApplyMemories(const std::vector<sync_pb::CompassMemory>& changed,
                     const std::set<std::string>& deleted) override {
    if (!memory_) {
      return;
    }
    std::vector<claude::MemoryEntry> entries;
    for (const sync_pb::CompassMemory& memory : changed) {
      claude::MemoryEntry& entry = entries.emplace_back();
      entry.id = memory.uuid();
      entry.text = memory.text();
      entry.created = base::Time::FromDeltaSinceWindowsEpoch(
          base::Microseconds(memory.create_time()));
      entry.updated = base::Time::FromDeltaSinceWindowsEpoch(
          base::Microseconds(memory.update_time()));
    }
    memory_->ApplyRemote(entries, deleted);
  }

  // claude::ClaudeMemory::Observer:
  void OnMemoriesChanged() override { bridge_->NotifyLocalChange(); }

  void DeleteSpace(SpaceModel& space) override {
    SpaceList& list = spaces_->list();
    if (list.spaces().size() > 1) {
      list.RemoveSpace(space);
    }
  }

  void MoveEntry(SidebarNode* node,
                 SpaceModel& target,
                 SidebarNode* parent,
                 size_t index) override {
    SpaceList& list = spaces_->list();
    SpaceModel* source = list.favorites().Contains(node)
                             ? &list.favorites()
                             : list.FindSpaceOf(node);
    if (!source) {
      return;
    }
    if (source == &target) {
      if (source->CanMove(node, parent)) {
        source->Move(node, parent, index);
      }
    } else if (source->CanMoveToSpace(node, target, parent)) {
      // The node keeps its ID, so its tab stays with it.
      source->MoveToSpace(node, target, parent, index);
    }
  }

  bool IsShownInFocusedWindow(const SidebarNode& entry) override {
    const int current = spaces_->current_tab();
    if (current == -1) {
      return false;
    }
    if (spaces_->TabFor(entry.id()) == current) {
      return true;
    }
    for (const auto& child : entry.children()) {
      if (spaces_->TabFor(child->id()) == current) {
        return true;
      }
    }
    return false;
  }

  bool IsInUse(const SidebarNode& entry) override {
    if (spaces_->TabFor(entry.id()) != -1) {
      return true;
    }
    for (const auto& child : entry.children()) {
      if (spaces_->TabFor(child->id()) != -1) {
        return true;
      }
    }
    return false;
  }

  void TransferLiveTab(const SidebarNode& from, SidebarNode& to) override {
    spaces_->TransferTab(from.id(), to.id());
  }

 private:
  const raw_ptr<SpacesAndroid> spaces_;
  std::unique_ptr<CompassSpacesSyncBridge> bridge_;
  base::WeakPtr<claude::ClaudeMemory> memory_;
};

// Runs COMPASS_SPACES only once the chain's passphrase is in place, as on the
// desktop (compass_spaces_sync.cc).
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

SpacesSyncHolder* GetHolder(Profile* profile) {
  SpacesAndroid* spaces = SpacesAndroid::Get(profile);
  // This is the only kind of attachment.
  auto* holder = static_cast<SpacesSyncHolder*>(spaces->sync_attachment());
  if (!holder) {
    auto owned = std::make_unique<SpacesSyncHolder>(profile, spaces);
    holder = owned.get();
    spaces->SetSyncAttachment(std::move(owned));
  }
  return holder;
}

base::WeakPtr<syncer::DataTypeControllerDelegate> GetControllerDelegate(
    Profile* profile) {
  return GetHolder(profile)
      ->bridge()
      .change_processor()
      ->GetControllerDelegate();
}

syncer::DataTypeController::TypeVector CreateControllers(
    content::BrowserContext* context,
    syncer::SyncService* sync_service) {
  Profile* profile = Profile::FromBrowserContext(context);
  const syncer::ProxyDataTypeControllerDelegate::DelegateProvider provider =
      base::BindRepeating(&GetControllerDelegate, base::Unretained(profile));
  const scoped_refptr<base::SequencedTaskRunner> runner =
      base::SequencedTaskRunner::GetCurrentDefault();
  syncer::DataTypeController::TypeVector controllers;
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

void NotifySpacesSyncOfLocalChange(Profile* profile) {
  SpacesAndroid* spaces =
      profile ? SpacesAndroid::GetIfExists(profile) : nullptr;
  auto* holder =
      spaces ? static_cast<SpacesSyncHolder*>(spaces->sync_attachment())
             : nullptr;
  if (holder) {
    holder->bridge().NotifyLocalChange();
  }
}

}  // namespace compass::sync
