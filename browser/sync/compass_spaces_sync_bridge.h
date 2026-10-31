// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_COMPASS_SPACES_SYNC_BRIDGE_H_
#define COMPASS_BROWSER_SYNC_COMPASS_SPACES_SYNC_BRIDGE_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"
#include "components/sync/base/unique_position.h"
#include "components/sync/model/data_type_store.h"
#include "components/sync/model/data_type_sync_bridge.h"

namespace syncer {
class MetadataBatch;
}  // namespace syncer

namespace compass::sync {

// Values of the int32 "enums" in compass_spaces_specifics.proto.
inline constexpr int kSectionPinned = 1;
inline constexpr int kSectionToday = 2;
inline constexpr int kSectionFavorites = 3;
inline constexpr int kSectionArchive = 4;
inline constexpr int kNodeTab = 1;
inline constexpr int kNodeFolder = 2;
inline constexpr int kNodeSplit = 3;
// CompassSpace.color is SpaceColor's value + 1 (0 = not set).

// Syncs a profile's spaces, their pinned trees, its favorites and folder
// collapse as COMPASS_SPACES (docs/design/sync.md, "The COMPASS_SPACES data
// type" and "Slice 3 notes"). One per profile, owned (through
// SpaceService::Attachment) by its SpaceService. Today and the archive aren't
// synced yet (slice 3b).
//
// It keeps a mirror: the specifics of every entity as the server last had
// them (from a remote change or our own commit), saved in its DataTypeStore
// with the sync metadata. Local changes aren't tracked one by one: any change
// of the spaces schedules a pass that computes each entity's specifics from
// the model and commits those that differ from the mirror, and deletes the
// mirror's entities the model no longer has. Remote changes go into the
// mirror first and then into the model, through its normal methods; the next
// pass then finds nothing to commit for them (no echo), only the fix-ups the
// model needed (a cycle or an invalid placement resolved to the section's
// root, a split left with one pane).
//
// Order among siblings (and of the spaces) is a syncer::UniquePosition, as
// bookmarks do: a pass keeps the positions of the longest run of siblings
// already in order and gives the others new ones between their neighbors, so
// a move commits one entity.
class CompassSpacesSyncBridge : public syncer::DataTypeSyncBridge,
                                public SpaceList::Observer,
                                public SpaceModel::Observer {
 public:
  // What needs windows and live tabs, which the bridge doesn't know about.
  // Without a delegate (tests), the bridge changes the model directly.
  class Delegate {
   public:
    virtual ~Delegate() = default;
    // Removes |space|; windows showing it switch away first. May leave it
    // (the last space).
    virtual void DeleteSpace(SpaceModel& space) = 0;
    // Moves |node| and its live tabs to another model (space or favorites),
    // into |parent| at |index|.
    virtual void MoveEntry(SidebarNode* node,
                           SpaceModel& target,
                           SidebarNode* parent,
                           size_t index) = 0;
  };

  // |list| is the profile's spaces; it must outlive the bridge. |delegate|
  // may be null.
  CompassSpacesSyncBridge(
      std::unique_ptr<syncer::DataTypeLocalChangeProcessor> change_processor,
      syncer::OnceDataTypeStoreFactory create_store,
      SpaceList* list,
      Delegate* delegate);
  CompassSpacesSyncBridge(const CompassSpacesSyncBridge&) = delete;
  CompassSpacesSyncBridge& operator=(const CompassSpacesSyncBridge&) = delete;
  ~CompassSpacesSyncBridge() override;

  // The list's file has loaded. Sync starts once it has and the store is
  // read.
  void OnModelLoaded();

  // Commits local changes now rather than in a task.
  void SendLocalChangesForTesting() { SendLocalChanges(); }
  bool ready_for_testing() const { return ready_; }

  // syncer::DataTypeSyncBridge:
  std::unique_ptr<syncer::MetadataChangeList> CreateMetadataChangeList()
      override;
  std::optional<syncer::ModelError> MergeFullSyncData(
      std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
      syncer::EntityChangeList entity_data) override;
  std::optional<syncer::ModelError> ApplyIncrementalSyncChanges(
      std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
      syncer::EntityChangeList entity_changes) override;
  std::unique_ptr<syncer::DataBatch> GetDataForCommit(
      StorageKeyList storage_keys) override;
  std::unique_ptr<syncer::DataBatch> GetAllDataForDebugging() override;
  std::string GetClientTag(const syncer::EntityData& entity_data) const override;
  std::string GetStorageKey(
      const syncer::EntityData& entity_data) const override;
  void ApplyDisableSyncChanges(std::unique_ptr<syncer::MetadataChangeList>
                                   delete_metadata_change_list) override;
  sync_pb::EntitySpecifics TrimAllSupportedFieldsFromRemoteSpecifics(
      const sync_pb::EntitySpecifics& entity_specifics) const override;
  bool IsEntityDataValid(const syncer::EntityData& entity_data) const override;

  // SpaceList::Observer:
  void OnSpaceAdded(SpaceModel& space) override;
  void OnSpaceUpdated(const SpaceModel& space) override;
  void OnSpaceWillBeRemoved(SpaceModel& space) override;
  void OnSpaceRemoved() override;
  void OnSpaceMoved(const SpaceModel& space) override;

  // SpaceModel::Observer:
  void OnNodeAdded(const SidebarNode& node) override;
  void OnNodeMoved(const SidebarNode& node,
                   const SidebarNode& old_parent,
                   size_t old_index) override;
  void OnNodeChanged(const SidebarNode& node) override;
  void OnNodeWillBeRemoved(const SidebarNode& node) override;
  void OnNodeWillMoveToSpace(const SidebarNode& node) override;

 private:
  using Mirror = std::map<std::string, sync_pb::CompassSpacesSpecifics>;

  void OnStoreCreated(const std::optional<syncer::ModelError>& error,
                      std::unique_ptr<syncer::DataTypeStore> store);
  void OnReadAllDataAndMetadata(
      const std::optional<syncer::ModelError>& error,
      std::unique_ptr<syncer::DataTypeStore::RecordList> records,
      std::unique_ptr<syncer::MetadataBatch> metadata);
  void MaybeStart();
  void Commit(std::unique_ptr<syncer::DataTypeStore::WriteBatch> batch);
  void OnCommitted(const std::optional<syncer::ModelError>& error);

  // Local changes.
  void ScheduleSendLocalChanges();
  void SendLocalChanges();
  // Puts and deletes what differs between the model and the mirror into
  // |batch|. Returns how many entities changed.
  size_t SendLocalChangesTo(syncer::DataTypeStore::WriteBatch& batch);
  // Each entity's specifics as the model has it.
  Mirror ComputeLocalSpecifics() const;
  void AddLocalNodes(const SidebarNode& parent,
                     const std::string& parent_key,
                     int section,
                     const std::string& space_key,
                     const std::set<std::string>& model_keys,
                     Mirror& out) const;
  // Whether a mirror entity the model doesn't have was deleted here (rather
  // than not applied: a type or section this version doesn't handle).
  bool WasDeletedLocally(const sync_pb::CompassSpacesSpecifics& specifics,
                         const Mirror& local) const;

  // Remote changes.
  std::optional<syncer::ModelError> ApplyRemote(
      std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
      syncer::EntityChangeList changes,
      bool initial_merge);
  // Returns the keys of the spaces it created.
  std::set<std::string> ApplySpaces(const std::vector<std::string>& keys,
                                    const std::set<std::string>& deleted);
  void OrderSpaces();
  // Initial merge: favorites the chain has (by URL) and an untouched default
  // space give way to the chain's.
  void MergeWithChain(const std::vector<std::string>& remote_nodes,
                      const std::set<std::string>& remote_spaces);
  void ApplyNodes(const std::vector<std::string>& keys,
                  const std::set<std::string>& deleted);
  void ApplyNodeDeletions(const std::set<std::string>& deleted);
  void PlaceNode(const std::string& key, const std::set<std::string>& deleted);
  void NormalizeSplits();

  // Model helpers.
  SidebarNode* FindNode(const base::Uuid& id) const;
  SpaceModel* ModelOf(const SidebarNode* node) const;
  // Whether |node| is in a section this slice syncs (pinned, favorites).
  bool IsSynced(const SidebarNode* node) const;
  // The model a remote node belongs in, or null if it isn't here (yet).
  SpaceModel* TargetModel(const sync_pb::CompassNode& node) const;
  void MoveNode(SidebarNode* node,
                SpaceModel& target,
                SidebarNode* parent,
                size_t index);
  void DeleteSpace(SpaceModel& space);
  // Where a node at |position| goes among |parent|'s children, |node| left
  // out.
  size_t InsertionIndex(const SidebarNode& parent,
                        const SidebarNode* node,
                        const sync_pb::UniquePosition& position) const;
  // A child's position in the mirror, if the mirror has it under |parent|.
  std::optional<syncer::UniquePosition> MirrorPosition(
      const SidebarNode& child,
      const SidebarNode& parent) const;
  std::string ParentKey(const SidebarNode& parent) const;
  void Observe(SpaceModel& model);

  const raw_ptr<SpaceList> list_;
  const raw_ptr<Delegate> delegate_;
  std::unique_ptr<syncer::DataTypeStore> store_;
  // Read before the model loaded.
  std::unique_ptr<syncer::MetadataBatch> pending_metadata_;
  bool store_loaded_ = false;
  bool model_loaded_ = false;
  bool ready_ = false;
  bool applying_remote_ = false;
  bool send_scheduled_ = false;
  Mirror mirror_;
  // Nodes parked at their section's root whose awaited parent was deleted:
  // they stay there, and the next pass commits it.
  std::set<std::string> unparked_;

  base::WeakPtrFactory<CompassSpacesSyncBridge> weak_factory_{this};
};

namespace internal {

// Positions for siblings |keys|, in order, given the positions they had
// (none for new ones or ones that came from elsewhere): the longest run
// already in order keeps its positions, the others get new ones between
// their neighbors. Exposed for tests.
std::vector<syncer::UniquePosition> AssignPositions(
    const std::vector<std::string>& keys,
    const std::vector<std::optional<syncer::UniquePosition>>& existing);

}  // namespace internal

}  // namespace compass::sync

#endif  // COMPASS_BROWSER_SYNC_COMPASS_SPACES_SYNC_BRIDGE_H_
