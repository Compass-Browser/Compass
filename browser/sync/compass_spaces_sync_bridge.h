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
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"
#include "components/sync/base/unique_position.h"
#include "components/sync/model/data_type_store.h"
#include "components/sync/model/data_type_sync_bridge.h"
#include "url/gurl.h"

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
// CompassSpace's theme fields: see compass_spaces_specifics.proto and
// ThemeToProto() in the .cc.

// A tab's URL, title and last-active time change as it's used; an entity
// whose only change is those commits at most this often. Structure (new,
// moved, closed, pinned, archived, renamed) commits at once.
inline constexpr base::TimeDelta kContentCommitInterval = base::Seconds(10);
// Archived entries leave the server (not this device) once they're older.
inline constexpr base::TimeDelta kArchiveSyncPeriod = base::Days(30);
// Last-active times commit only once they've moved by this much: the archive
// delays they drive are hours.
inline constexpr base::TimeDelta kLastActiveGranularity = base::Minutes(1);

// Favicons (docs/design/sync.md, "Favicons"): a larger icon always replaces
// the account's; a different one of the same size only once the account's is
// this old.
inline constexpr base::TimeDelta kFaviconRefreshAge = base::Days(7);
// An icon no entry of the account points at goes once it's this old (an
// initial download may bring it a batch before its entries).
inline constexpr base::TimeDelta kFaviconGracePeriod = base::Days(1);
// The PNG's limits.
inline constexpr size_t kFaviconMaxBytes = 8 * 1024;
inline constexpr int kFaviconMaxPixels = 64;

// A favicon entity's UUID (and client tag) for |host|: the first 16 bytes of
// SHA-256("compass-favicon:" + lowercase host), shaped as a version-5 UUID.
std::string FaviconKey(std::string_view host);

// A space of the switcher (SpaceDirectory), of any profile, and the directory
// position its profile's bridge last synced for it, if known (slice 5).
struct DirectorySlot {
  DirectorySlot();
  DirectorySlot(std::string key,
                std::optional<syncer::UniquePosition> position);
  DirectorySlot(const DirectorySlot&);
  DirectorySlot& operator=(const DirectorySlot&);
  ~DirectorySlot();

  std::string key;
  std::optional<syncer::UniquePosition> position;
};

// Syncs a profile's spaces, their pinned trees and Today, their archives, its
// favorites and folder collapse as COMPASS_SPACES (docs/design/sync.md, "The
// COMPASS_SPACES data type", "Today across devices", "Slice 3 notes" and
// "Slice 3b notes"). One per profile, owned (through SpaceService::Attachment)
// by its SpaceService. New tab and blank pages in Today stay local. The home
// profile's also carries the chain's profile list ("Slice 5 notes").
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
    // Whether |entry| (a Today tab or split) is the tab the focused window
    // shows: another device closing it doesn't close it here.
    virtual bool IsShownInFocusedWindow(const SidebarNode& entry);
    // Whether |entry| is shown in a window or plays sound, as the archiver
    // spares: another device archiving it doesn't archive it here.
    virtual bool IsInUse(const SidebarNode& entry);
    // Hands |from|'s live tab, if it has one, to |to|, if it has none.
    virtual void TransferLiveTab(const SidebarNode& from, SidebarNode& to) {}

    // Profiles (slice 5; docs/design/sync.md, "Slice 5 notes"). The defaults
    // are one profile that carries no profile list.
    //
    // The chain's profiles this device has, as entities, if this bridge
    // carries the chain's profile list (the home profile's, once its
    // chain is loaded); nullopt otherwise.
    virtual std::optional<std::vector<sync_pb::CompassProfile>>
    GetLocalProfiles();
    // Every CompassProfile entity the bridge has, after it started and after
    // a remote change brought one. Called while applying, before the pass
    // that follows, so a profile renamed here doesn't echo.
    virtual void OnChainProfiles(
        const std::vector<sync_pb::CompassProfile>& profiles);
    // The sync UUID of the bridge's profile, put on its spaces. Empty: left
    // as received.
    virtual std::string GetProfileUuid();
    // Space |id| came from the server but isn't this profile's here; if
    // another profile here has it, it was moved between profiles on another
    // device: moves it here with its entries (MoveSpaceToProfile()) and
    // returns it. Null if no other profile has it.
    virtual SpaceModel* AdoptSpace(const base::Uuid& id);
    // The switcher's spaces of every profile, in order, with the positions
    // the other profiles' bridges synced for theirs, so this one's positions
    // fall between them. Empty: only this profile's spaces count.
    virtual std::vector<DirectorySlot> GetDirectoryOrder();
    // This profile's spaces' directory positions as synced, after a pass
    // (|remote| false) or a remote change (true).
    virtual void OnSpacePositions(
        const std::map<std::string, syncer::UniquePosition>& positions,
        bool remote);
    // Synced icons for hosts entries point at, after a remote change brought
    // the icons or entries on their hosts (and after the initial merge):
    // the delegate shows them for entries whose page has no icon here.
    virtual void ApplyFavicons(
        const std::vector<sync_pb::CompassFavicon>& favicons) {}
    // Sync runs (after the initial merge, or at startup if it already ran):
    // the delegate offers the icons of the pages open now (OfferFavicon()).
    virtual void OnFaviconsWritable() {}
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

  // Something the bridge doesn't observe changed (the switcher's order, the
  // profiles): a pass looks for what to commit.
  void NotifyLocalChange();

  // The URLs of this profile's entries, in every section, archives and
  // favorites included, by host (lowercase).
  std::map<std::string, std::vector<GURL>> GetEntryUrlsByHost() const;
  std::set<std::string> GetEntryHosts() const;
  // An icon this device took from a page it loaded on |host|: a PNG of at
  // most kFaviconMaxPixels a side and kFaviconMaxBytes, from a source whose
  // larger side was |source_size| px. Commits it if an entry points at the
  // host and the account has no icon for it, a smaller one, or a different
  // one of the same size at least kFaviconRefreshAge old. Returns whether it
  // committed.
  bool OfferFavicon(const std::string& host,
                    const std::string& png,
                    int source_size);

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
  // Upstream's choice (a local change wins over a remote delete), logged:
  // the entity stays here and goes up again (slice 7).
  syncer::ConflictResolution ResolveConflict(
      const std::string& storage_key,
      const syncer::EntityData& remote_data) const override;

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
  void OnNodeLastActiveChanged(const SidebarNode& node) override;
  void OnArchiveChanged(const SpaceModel& space) override;

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
  // Directory positions for this profile's spaces |keys|, in the list's
  // order, which had |existing|: among every profile's spaces if the
  // delegate gives their order, else among these alone.
  std::vector<syncer::UniquePosition> AssignSpacePositions(
      const std::vector<std::string>& keys,
      const std::vector<std::optional<syncer::UniquePosition>>& existing) const;
  // The profile entities of |profiles| (GetLocalProfiles()) into |out|.
  void AddLocalProfiles(const std::vector<sync_pb::CompassProfile>& profiles,
                        Mirror& out) const;
  // The CompassProfile entities of the mirror.
  std::vector<sync_pb::CompassProfile> MirrorProfiles() const;
  // Hands the spaces' synced positions to the delegate.
  void ReportSpacePositions(bool remote);
  void AddLocalNodes(const SidebarNode& parent,
                     const std::string& parent_key,
                     int section,
                     const std::string& space_key,
                     const std::set<std::string>& model_keys,
                     Mirror& out) const;
  // |space|'s archived entries of the last kArchiveSyncPeriod.
  void AddLocalArchive(const SpaceModel& space, Mirror& out) const;
  // Whether a Today node syncs: new tab and blank pages (and splits with
  // one) don't, unless they did before.
  bool IsSyncableToday(const SidebarNode& node) const;
  // Whether a mirror entity the model doesn't have was deleted here (rather
  // than not applied: a type or section this version doesn't handle, or an
  // archived split whose panes haven't come).
  bool WasDeletedLocally(const sync_pb::CompassSpacesSpecifics& specifics,
                         const Mirror& local) const;
  // Whether the mirror's favicon entity |favicon| goes: no entry points at
  // its host (|hosts|) and it's older than kFaviconGracePeriod.
  bool IsFaviconGarbage(const sync_pb::CompassFavicon& favicon,
                        const std::set<std::string>& hosts) const;
  // Hands the delegate the mirror's icons for |hosts| (all with nullopt).
  void ApplyMirrorFavicons(const std::optional<std::set<std::string>>& hosts);
  // Whether the archived entity |node| has what it needs to show: a split
  // its panes, a pane its split.
  bool IsArchiveComplete(const sync_pb::CompassNode& node) const;
  // Runs a pass at |when|, unless one is due earlier.
  void ScheduleThrottledSend(base::TimeTicks when);
  // Runs a pass when the oldest archived entry turns kArchiveSyncPeriod old.
  void ScheduleArchiveExpiry();

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
  // Removes deleted nodes and archived entries. Archived entries a deleted
  // pane belonged to go into |archive_roots|, to be rebuilt.
  void ApplyNodeDeletions(const std::set<std::string>& deleted,
                          std::set<std::string>& archive_roots);
  void PlaceNode(const std::string& key, const std::set<std::string>& deleted);
  void NormalizeSplits();
  // Archiving on another device: the archived entries |roots| (their keys)
  // go into their space's archive, from the mirror, and leave Today, unless
  // in use here.
  void ApplyArchives(const std::set<std::string>& roots);
  // |key| is back from the archive (or never was there): its archived entry
  // goes.
  void DropFromArchives(const std::string& key);
  // Initial merge: a Today tab of this device whose URL the chain's Today of
  // the same space has gives way to the chain's.
  void DedupeToday(const std::vector<std::string>& remote_nodes);
  // Replaces |node| (and its subtree) with a copy with fresh IDs, in its
  // place, with its live tabs. Returns the copy.
  SidebarNode* Recreate(SidebarNode* node);
  SidebarNode* CopyNode(SpaceModel& model,
                        const SidebarNode& node,
                        SidebarNode* parent,
                        size_t index);

  // Model helpers.
  SidebarNode* FindNode(const base::Uuid& id) const;
  SpaceModel* ModelOf(const SidebarNode* node) const;
  // Whether |node| is in a space or the favorites.
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
  // When each entity was last committed from here.
  std::map<std::string, base::TimeTicks> last_commit_;
  // Entities whose content changed here but waits for kContentCommitInterval.
  // A remote change doesn't overwrite their URL and title: they commit next.
  std::set<std::string> throttled_;
  base::OneShotTimer throttle_timer_;
  base::OneShotTimer archive_expiry_timer_;

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
