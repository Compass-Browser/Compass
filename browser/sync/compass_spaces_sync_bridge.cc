// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/compass_spaces_sync_bridge.h"

#include <algorithm>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "components/sync/base/client_tag_hash.h"
#include "components/sync/base/data_type.h"
#include "components/sync/base/deletion_origin.h"
#include "components/sync/model/data_type_local_change_processor.h"
#include "components/sync/model/entity_change.h"
#include "components/sync/model/metadata_batch.h"
#include "components/sync/model/metadata_change_list.h"
#include "components/sync/model/mutable_data_batch.h"
#include "components/sync/protocol/entity_data.h"
#include "components/sync/protocol/entity_specifics.pb.h"
#include "components/sync/protocol/unique_position.pb.h"
#include "url/gurl.h"

namespace compass::sync {

namespace {

using Type = SidebarNode::Type;

std::string Key(const base::Uuid& id) {
  return id.AsLowercaseString();
}

base::Uuid ParseKey(const std::string& key) {
  return base::Uuid::ParseLowercase(key);
}

syncer::UniquePosition::Suffix SuffixFor(const std::string& key) {
  return syncer::UniquePosition::GenerateSuffix(
      syncer::ClientTagHash::FromUnhashed(syncer::COMPASS_SPACES, key));
}

std::optional<syncer::UniquePosition> ValidPosition(
    const sync_pb::UniquePosition& proto) {
  syncer::UniquePosition position = syncer::UniquePosition::FromProto(proto);
  if (!position.IsValid()) {
    return std::nullopt;
  }
  return position;
}

std::optional<Type> TypeFromProto(int type) {
  switch (type) {
    case kNodeTab:
      return Type::kTab;
    case kNodeFolder:
      return Type::kFolder;
    case kNodeSplit:
      return Type::kSplit;
    default:
      return std::nullopt;
  }
}

int TypeToProto(Type type) {
  switch (type) {
    case Type::kTab:
      return kNodeTab;
    case Type::kFolder:
      return kNodeFolder;
    case Type::kSplit:
      return kNodeSplit;
    case Type::kRoot:
      break;
  }
  return 0;
}

// Sets a string field to |value|, or clears it for an empty one. Leaves it
// alone if it already reads as |value|, so an explicitly empty field from
// another client doesn't count as a change.
#define COMPASS_SET_STRING(proto, field, value) \
  do {                                          \
    const std::string& v = (value);             \
    if ((proto).field() != v) {                 \
      if (v.empty()) {                          \
        (proto).clear_##field();                \
      } else {                                  \
        (proto).set_##field(v);                 \
      }                                         \
    }                                           \
  } while (false)

// The node's own fields: what it shows, not where it is.
void FillNodeContent(const SidebarNode& node, sync_pb::CompassNode& proto) {
  const bool tab = node.type() == Type::kTab;
  COMPASS_SET_STRING(proto, url,
                     tab ? node.url().possibly_invalid_spec() : std::string());
  COMPASS_SET_STRING(proto, title, base::UTF16ToUTF8(node.title()));
  COMPASS_SET_STRING(proto, custom_title,
                     tab ? base::UTF16ToUTF8(node.custom_title())
                         : std::string());
  const bool collapsed = node.type() == Type::kFolder && node.collapsed();
  if (proto.collapsed() != collapsed) {
    if (collapsed) {
      proto.set_collapsed(true);
    } else {
      proto.clear_collapsed();
    }
  }
}

void CollectKeys(const SidebarNode& node, std::set<std::string>& keys) {
  for (const auto& child : node.children()) {
    keys.insert(Key(child->id()));
    CollectKeys(*child, keys);
  }
}

void CollectSplits(const SidebarNode& node, std::vector<base::Uuid>& splits) {
  for (const auto& child : node.children()) {
    if (child->type() == Type::kSplit) {
      splits.push_back(child->id());
    }
    CollectSplits(*child, splits);
  }
}

}  // namespace

namespace internal {

std::vector<syncer::UniquePosition> AssignPositions(
    const std::vector<std::string>& keys,
    const std::vector<std::optional<syncer::UniquePosition>>& existing) {
  CHECK_EQ(keys.size(), existing.size());
  const size_t n = keys.size();
  // The longest run of existing positions already in increasing order.
  // Quadratic, but sibling lists are short.
  std::vector<size_t> length(n, 0);
  std::vector<std::optional<size_t>> previous(n);
  std::optional<size_t> best;
  for (size_t i = 0; i < n; ++i) {
    if (!existing[i] || !existing[i]->IsValid()) {
      continue;
    }
    length[i] = 1;
    for (size_t j = 0; j < i; ++j) {
      if (length[j] > 0 && existing[j]->LessThan(*existing[i]) &&
          length[j] + 1 > length[i]) {
        length[i] = length[j] + 1;
        previous[i] = j;
      }
    }
    if (!best || length[i] > length[*best]) {
      best = i;
    }
  }
  std::vector<bool> keep(n, false);
  for (std::optional<size_t> i = best; i; i = previous[*i]) {
    keep[*i] = true;
  }

  std::vector<syncer::UniquePosition> positions(n);
  for (size_t i = 0; i < n; ++i) {
    if (keep[i]) {
      positions[i] = *existing[i];
      continue;
    }
    std::optional<syncer::UniquePosition> next;
    for (size_t j = i + 1; j < n; ++j) {
      if (keep[j]) {
        next = *existing[j];
        break;
      }
    }
    const syncer::UniquePosition::Suffix suffix = SuffixFor(keys[i]);
    if (i > 0 && next) {
      positions[i] =
          syncer::UniquePosition::Between(positions[i - 1], *next, suffix);
    } else if (i > 0) {
      positions[i] = syncer::UniquePosition::After(positions[i - 1], suffix);
    } else if (next) {
      positions[i] = syncer::UniquePosition::Before(*next, suffix);
    } else {
      positions[i] = syncer::UniquePosition::InitialPosition(suffix);
    }
  }
  return positions;
}

}  // namespace internal

DirectorySlot::DirectorySlot() = default;
DirectorySlot::DirectorySlot(std::string key,
                             std::optional<syncer::UniquePosition> position)
    : key(std::move(key)), position(std::move(position)) {}
DirectorySlot::DirectorySlot(const DirectorySlot&) = default;
DirectorySlot& DirectorySlot::operator=(const DirectorySlot&) = default;
DirectorySlot::~DirectorySlot() = default;

std::optional<std::vector<sync_pb::CompassProfile>>
CompassSpacesSyncBridge::Delegate::GetLocalProfiles() {
  return std::nullopt;
}

void CompassSpacesSyncBridge::Delegate::OnChainProfiles(
    const std::vector<sync_pb::CompassProfile>& profiles) {}

std::string CompassSpacesSyncBridge::Delegate::GetProfileUuid() {
  return std::string();
}

SpaceModel* CompassSpacesSyncBridge::Delegate::AdoptSpace(
    const base::Uuid& id) {
  return nullptr;
}

std::vector<DirectorySlot>
CompassSpacesSyncBridge::Delegate::GetDirectoryOrder() {
  return {};
}

void CompassSpacesSyncBridge::Delegate::OnSpacePositions(
    const std::map<std::string, syncer::UniquePosition>& positions,
    bool remote) {}

CompassSpacesSyncBridge::CompassSpacesSyncBridge(
    std::unique_ptr<syncer::DataTypeLocalChangeProcessor> change_processor,
    syncer::OnceDataTypeStoreFactory create_store,
    SpaceList* list,
    Delegate* delegate)
    : syncer::DataTypeSyncBridge(std::move(change_processor)),
      list_(list),
      delegate_(delegate) {
  CHECK(list_);
  list_->AddObserver(this);
  Observe(list_->favorites());
  for (const auto& space : list_->spaces()) {
    Observe(*space);
  }
  std::move(create_store)
      .Run(syncer::COMPASS_SPACES,
           base::BindOnce(&CompassSpacesSyncBridge::OnStoreCreated,
                          weak_factory_.GetWeakPtr()));
}

CompassSpacesSyncBridge::~CompassSpacesSyncBridge() {
  for (const auto& space : list_->spaces()) {
    space->RemoveObserver(this);
  }
  list_->favorites().RemoveObserver(this);
  list_->RemoveObserver(this);
}

void CompassSpacesSyncBridge::OnModelLoaded() {
  model_loaded_ = true;
  MaybeStart();
}

void CompassSpacesSyncBridge::NotifyLocalChange() {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnStoreCreated(
    const std::optional<syncer::ModelError>& error,
    std::unique_ptr<syncer::DataTypeStore> store) {
  if (error) {
    change_processor()->ReportError(*error);
    return;
  }
  store_ = std::move(store);
  store_->ReadAllDataAndMetadata(
      base::BindOnce(&CompassSpacesSyncBridge::OnReadAllDataAndMetadata,
                     weak_factory_.GetWeakPtr()));
}

void CompassSpacesSyncBridge::OnReadAllDataAndMetadata(
    const std::optional<syncer::ModelError>& error,
    std::unique_ptr<syncer::DataTypeStore::RecordList> records,
    std::unique_ptr<syncer::MetadataBatch> metadata) {
  if (error) {
    change_processor()->ReportError(*error);
    return;
  }
  for (const syncer::DataTypeStore::Record& record : *records) {
    sync_pb::CompassSpacesSpecifics specifics;
    // A record that doesn't parse is dropped; the next pass puts it back if
    // the model has it.
    if (specifics.ParseFromString(record.value)) {
      mirror_[record.id] = std::move(specifics);
    }
  }
  pending_metadata_ = std::move(metadata);
  store_loaded_ = true;
  MaybeStart();
}

void CompassSpacesSyncBridge::MaybeStart() {
  if (ready_ || !store_loaded_ || !model_loaded_) {
    return;
  }
  ready_ = true;
  VLOG(1) << "[compass-sync] spaces: model ready, " << mirror_.size()
          << " synced entities, " << list_->spaces().size() << " spaces";
  change_processor()->ModelReadyToSync(std::move(pending_metadata_));
  if (delegate_) {
    // What the last run had: the switcher's order and the profile list
    // don't wait for the server.
    ReportSpacePositions(/*remote=*/false);
    std::vector<sync_pb::CompassProfile> profiles = MirrorProfiles();
    if (!profiles.empty()) {
      delegate_->OnChainProfiles(profiles);
    }
  }
  // Changes made while sync wasn't running.
  ScheduleSendLocalChanges();
}

void CompassSpacesSyncBridge::Commit(
    std::unique_ptr<syncer::DataTypeStore::WriteBatch> batch) {
  store_->CommitWriteBatch(
      std::move(batch), base::BindOnce(&CompassSpacesSyncBridge::OnCommitted,
                                       weak_factory_.GetWeakPtr()));
}

void CompassSpacesSyncBridge::OnCommitted(
    const std::optional<syncer::ModelError>& error) {
  if (error) {
    change_processor()->ReportError(*error);
  }
}

// Local changes ---------------------------------------------------------------

void CompassSpacesSyncBridge::ScheduleSendLocalChanges() {
  if (send_scheduled_ || !ready_) {
    return;
  }
  send_scheduled_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&CompassSpacesSyncBridge::SendLocalChanges,
                                weak_factory_.GetWeakPtr()));
}

void CompassSpacesSyncBridge::SendLocalChanges() {
  send_scheduled_ = false;
  if (!ready_ || !store_ || !change_processor()->IsTrackingMetadata()) {
    return;
  }
  std::unique_ptr<syncer::DataTypeStore::WriteBatch> batch =
      store_->CreateWriteBatch();
  if (SendLocalChangesTo(*batch) > 0) {
    Commit(std::move(batch));
  }
}

size_t CompassSpacesSyncBridge::SendLocalChangesTo(
    syncer::DataTypeStore::WriteBatch& batch) {
  Mirror local = ComputeLocalSpecifics();
  // The home profile's bridge also has the chain's profiles (slice 5).
  const std::optional<std::vector<sync_pb::CompassProfile>> profiles =
      delegate_ ? delegate_->GetLocalProfiles() : std::nullopt;
  if (profiles) {
    AddLocalProfiles(*profiles, local);
  }
  size_t puts = 0;
  for (const auto& [key, specifics] : local) {
    std::string serialized = specifics.SerializeAsString();
    auto it = mirror_.find(key);
    if (it != mirror_.end() && it->second.SerializeAsString() == serialized) {
      continue;
    }
    auto data = std::make_unique<syncer::EntityData>();
    // The name isn't needed, and goes to the server; the ID is enough.
    data->name = key;
    *data->specifics.mutable_compass_spaces() = specifics;
    change_processor()->Put(key, std::move(data),
                            batch.GetMetadataChangeList());
    batch.WriteData(key, serialized);
    mirror_[key] = specifics;
    ++puts;
  }

  std::vector<std::string> gone;
  for (const auto& [key, specifics] : mirror_) {
    if (local.contains(key)) {
      continue;
    }
    if (specifics.has_profile()) {
      // Profiles aren't deleted through sync (deleting one stays on its
      // device), but a home entity this device's home no longer has is one
      // it gave up for the chain's (two devices that each started as home).
      if (profiles && specifics.profile().home()) {
        gone.push_back(key);
      }
      continue;
    }
    if (WasDeletedLocally(specifics, local)) {
      gone.push_back(key);
    }
  }
  for (const std::string& key : gone) {
    change_processor()->Delete(key, syncer::DeletionOrigin::Unspecified(),
                               batch.GetMetadataChangeList());
    batch.DeleteData(key);
    mirror_.erase(key);
  }
  std::erase_if(unparked_, [this](const std::string& key) {
    auto it = mirror_.find(key);
    return it == mirror_.end() || !it->second.has_node() ||
           it->second.node().parent_uuid().empty();
  });

  if (puts > 0 || !gone.empty()) {
    VLOG(1) << "[compass-sync] spaces: committing " << puts << " changed, "
            << gone.size() << " deleted";
  }
  ReportSpacePositions(/*remote=*/false);
  return puts + gone.size();
}

CompassSpacesSyncBridge::Mirror
CompassSpacesSyncBridge::ComputeLocalSpecifics() const {
  Mirror local;

  // Spaces, in the list's order (the profile's order in the switcher; see
  // compass_spaces_sync.cc).
  std::vector<std::string> keys;
  std::vector<std::optional<syncer::UniquePosition>> existing;
  std::vector<const SpaceModel*> spaces;
  for (const auto& space : list_->spaces()) {
    const std::string key = Key(space->id());
    if (std::ranges::contains(keys, key)) {
      continue;
    }
    std::optional<syncer::UniquePosition> position;
    auto it = mirror_.find(key);
    if (it != mirror_.end() && it->second.has_space() &&
        it->second.space().has_directory_position()) {
      position = ValidPosition(it->second.space().directory_position());
    }
    keys.push_back(key);
    existing.push_back(position);
    spaces.push_back(space.get());
  }
  const std::vector<syncer::UniquePosition> positions =
      AssignSpacePositions(keys, existing);
  const std::string profile_uuid =
      delegate_ ? delegate_->GetProfileUuid() : std::string();
  for (size_t i = 0; i < spaces.size(); ++i) {
    sync_pb::CompassSpacesSpecifics specifics;
    auto it = mirror_.find(keys[i]);
    if (it != mirror_.end() && it->second.has_space()) {
      // Keeps what this version doesn't know (icon, profile).
      specifics = it->second;
    }
    sync_pb::CompassSpace& space = *specifics.mutable_space();
    const SpaceModel& model = *spaces[i];
    COMPASS_SET_STRING(space, uuid, keys[i]);
    COMPASS_SET_STRING(space, name, base::UTF16ToUTF8(model.name()));
    const int color = static_cast<int>(model.color()) + 1;
    if (space.color() != color) {
      space.set_color(color);
    }
    const int64_t archive_after = model.archive_after().InSeconds();
    if (!space.has_archive_after_seconds() ||
        space.archive_after_seconds() != archive_after) {
      space.set_archive_after_seconds(archive_after);
    }
    if (!existing[i] || !existing[i]->Equals(positions[i])) {
      *space.mutable_directory_position() = positions[i].ToProto();
    }
    if (!profile_uuid.empty()) {
      COMPASS_SET_STRING(space, profile_uuid, profile_uuid);
    }
    local[keys[i]] = std::move(specifics);
  }

  // Nodes: each space's pinned tree, then the favorites.
  std::set<std::string> model_keys;
  for (const auto& space : list_->spaces()) {
    CollectKeys(*space->pinned_root(), model_keys);
  }
  CollectKeys(*list_->favorites().pinned_root(), model_keys);
  for (const SpaceModel* space : spaces) {
    AddLocalNodes(*space->pinned_root(), std::string(), kSectionPinned,
                  Key(space->id()), model_keys, local);
  }
  AddLocalNodes(*list_->favorites().pinned_root(), std::string(),
                kSectionFavorites, std::string(), model_keys, local);
  return local;
}

std::vector<syncer::UniquePosition>
CompassSpacesSyncBridge::AssignSpacePositions(
    const std::vector<std::string>& keys,
    const std::vector<std::optional<syncer::UniquePosition>>& existing) const {
  const std::vector<DirectorySlot> order =
      delegate_ ? delegate_->GetDirectoryOrder() : std::vector<DirectorySlot>();
  // Every profile's spaces, this one's with the positions of its mirror,
  // the others' with what their bridges synced: positions given here fall
  // between the other accounts' (docs/design/sync.md, "Profiles").
  std::vector<std::string> all_keys;
  std::vector<std::optional<syncer::UniquePosition>> all_existing;
  std::vector<size_t> own;
  for (const DirectorySlot& slot : order) {
    auto it = std::ranges::find(keys, slot.key);
    if (it == keys.end()) {
      if (!std::ranges::contains(all_keys, slot.key)) {
        all_keys.push_back(slot.key);
        all_existing.push_back(slot.position);
      }
      continue;
    }
    // This profile's spaces must come in the list's order, each once
    // (compass_spaces_sync.cc keeps the two orders equal); else the order is
    // being changed and only this profile's spaces count.
    if (static_cast<size_t>(it - keys.begin()) != own.size()) {
      own.clear();
      break;
    }
    all_existing.push_back(existing[own.size()]);
    own.push_back(all_keys.size());
    all_keys.push_back(slot.key);
  }
  if (keys.empty() || own.size() != keys.size()) {
    return internal::AssignPositions(keys, existing);
  }
  const std::vector<syncer::UniquePosition> all =
      internal::AssignPositions(all_keys, all_existing);
  std::vector<syncer::UniquePosition> positions;
  for (size_t index : own) {
    positions.push_back(all[index]);
  }
  return positions;
}

void CompassSpacesSyncBridge::AddLocalProfiles(
    const std::vector<sync_pb::CompassProfile>& profiles,
    Mirror& out) const {
  for (const sync_pb::CompassProfile& profile : profiles) {
    const std::string& key = profile.uuid();
    if (!ParseKey(key).is_valid() || out.contains(key)) {
      continue;
    }
    sync_pb::CompassSpacesSpecifics specifics;
    auto it = mirror_.find(key);
    if (it != mirror_.end() && it->second.has_profile()) {
      // Keeps what this version doesn't know.
      specifics = it->second;
    }
    sync_pb::CompassProfile& entity = *specifics.mutable_profile();
    COMPASS_SET_STRING(entity, uuid, key);
    COMPASS_SET_STRING(entity, name, profile.name());
    if (entity.avatar() != profile.avatar()) {
      if (profile.avatar() > 0) {
        entity.set_avatar(profile.avatar());
      } else {
        entity.clear_avatar();
      }
    }
    if (entity.home() != profile.home()) {
      if (profile.home()) {
        entity.set_home(true);
      } else {
        entity.clear_home();
      }
    }
    out[key] = std::move(specifics);
  }
}

std::vector<sync_pb::CompassProfile> CompassSpacesSyncBridge::MirrorProfiles()
    const {
  std::vector<sync_pb::CompassProfile> profiles;
  for (const auto& [key, specifics] : mirror_) {
    if (specifics.has_profile()) {
      profiles.push_back(specifics.profile());
    }
  }
  return profiles;
}

void CompassSpacesSyncBridge::ReportSpacePositions(bool remote) {
  if (!delegate_) {
    return;
  }
  std::map<std::string, syncer::UniquePosition> positions;
  for (const auto& space : list_->spaces()) {
    const std::string key = Key(space->id());
    auto it = mirror_.find(key);
    if (it == mirror_.end() || !it->second.has_space() ||
        !it->second.space().has_directory_position()) {
      continue;
    }
    if (std::optional<syncer::UniquePosition> position =
            ValidPosition(it->second.space().directory_position())) {
      positions.emplace(key, *position);
    }
  }
  delegate_->OnSpacePositions(positions, remote);
}

void CompassSpacesSyncBridge::AddLocalNodes(
    const SidebarNode& parent,
    const std::string& parent_key,
    int section,
    const std::string& space_key,
    const std::set<std::string>& model_keys,
    Mirror& out) const {
  const bool at_root = parent.type() == Type::kRoot;
  std::vector<const SidebarNode*> included;
  std::vector<const SidebarNode*> placed;
  std::vector<std::string> keys;
  std::vector<std::optional<syncer::UniquePosition>> existing;
  for (const auto& child : parent.children()) {
    const std::string key = Key(child->id());
    if (out.contains(key)) {
      // Another node has this ID (a damaged file): only the first syncs.
      continue;
    }
    auto it = mirror_.find(key);
    const sync_pb::CompassNode* synced =
        it != mirror_.end() && it->second.has_node() ? &it->second.node()
                                                     : nullptr;
    const bool same_place = synced && synced->section() == section &&
                            synced->space_uuid() == space_key;
    if (at_root && same_place && !synced->parent_uuid().empty() &&
        !model_keys.contains(synced->parent_uuid()) &&
        !unparked_.contains(key)) {
      // Parked: it waits here for its parent, and keeps the place the
      // server has for it.
      sync_pb::CompassSpacesSpecifics specifics = it->second;
      FillNodeContent(*child, *specifics.mutable_node());
      out[key] = std::move(specifics);
      included.push_back(child.get());
      continue;
    }
    std::optional<syncer::UniquePosition> position;
    if (same_place && synced->parent_uuid() == parent_key &&
        synced->has_position()) {
      position = ValidPosition(synced->position());
    }
    // Reserves the key against duplicates further down.
    out[key];
    included.push_back(child.get());
    placed.push_back(child.get());
    keys.push_back(key);
    existing.push_back(position);
  }

  const std::vector<syncer::UniquePosition> positions =
      internal::AssignPositions(keys, existing);
  for (size_t i = 0; i < placed.size(); ++i) {
    sync_pb::CompassSpacesSpecifics specifics;
    auto it = mirror_.find(keys[i]);
    if (it != mirror_.end() && it->second.has_node()) {
      // Keeps what this version doesn't know.
      specifics = it->second;
    }
    sync_pb::CompassNode& node = *specifics.mutable_node();
    COMPASS_SET_STRING(node, uuid, keys[i]);
    COMPASS_SET_STRING(node, space_uuid, space_key);
    COMPASS_SET_STRING(node, parent_uuid, parent_key);
    if (node.section() != section) {
      node.set_section(section);
    }
    const int type = TypeToProto(placed[i]->type());
    if (node.type() != type) {
      node.set_type(type);
    }
    FillNodeContent(*placed[i], node);
    if (!existing[i] || !existing[i]->Equals(positions[i])) {
      *node.mutable_position() = positions[i].ToProto();
    }
    out[keys[i]] = std::move(specifics);
  }

  for (const SidebarNode* child : included) {
    if (!child->children().empty()) {
      AddLocalNodes(*child, Key(child->id()), section, space_key, model_keys,
                    out);
    }
  }
}

bool CompassSpacesSyncBridge::WasDeletedLocally(
    const sync_pb::CompassSpacesSpecifics& specifics,
    const Mirror& local) const {
  if (specifics.has_profile()) {
    // SendLocalChangesTo() decides.
    return false;
  }
  if (specifics.has_space()) {
    return true;
  }
  if (!specifics.has_node() || !TypeFromProto(specifics.node().type())) {
    return false;
  }
  const sync_pb::CompassNode& node = specifics.node();
  switch (node.section()) {
    case kSectionPinned: {
      // Its space is here, or was and goes too. A space this device never
      // had (another profile's, later) means it was never applied.
      if (local.contains(node.space_uuid())) {
        return true;
      }
      auto it = mirror_.find(node.space_uuid());
      return it != mirror_.end() && it->second.has_space();
    }
    case kSectionFavorites:
      return node.type() == kNodeTab;
    default:
      // Today and the archive: not this slice's.
      return false;
  }
}

// Remote changes --------------------------------------------------------------

std::unique_ptr<syncer::MetadataChangeList>
CompassSpacesSyncBridge::CreateMetadataChangeList() {
  return syncer::DataTypeStore::WriteBatch::CreateMetadataChangeList();
}

std::optional<syncer::ModelError> CompassSpacesSyncBridge::MergeFullSyncData(
    std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
    syncer::EntityChangeList entity_data) {
  return ApplyRemote(std::move(metadata_change_list), std::move(entity_data),
                     /*initial_merge=*/true);
}

std::optional<syncer::ModelError>
CompassSpacesSyncBridge::ApplyIncrementalSyncChanges(
    std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
    syncer::EntityChangeList entity_changes) {
  return ApplyRemote(std::move(metadata_change_list),
                     std::move(entity_changes), /*initial_merge=*/false);
}

std::optional<syncer::ModelError> CompassSpacesSyncBridge::ApplyRemote(
    std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
    syncer::EntityChangeList changes,
    bool initial_merge) {
  CHECK(store_);
  std::unique_ptr<syncer::DataTypeStore::WriteBatch> batch =
      store_->CreateWriteBatch(std::move(metadata_change_list));

  // The mirror first: what the model then does with them matches it, so it
  // isn't committed back.
  std::vector<std::string> spaces;
  std::vector<std::string> nodes;
  std::set<std::string> deleted;
  bool profiles_changed = false;
  for (const std::unique_ptr<syncer::EntityChange>& change : changes) {
    const std::string& key = change->storage_key();
    if (change->type() == syncer::EntityChange::ACTION_DELETE) {
      auto it = mirror_.find(key);
      if (it != mirror_.end() && it->second.has_profile()) {
        profiles_changed = true;
      }
      mirror_.erase(key);
      unparked_.erase(key);
      batch->DeleteData(key);
      deleted.insert(key);
      continue;
    }
    const sync_pb::CompassSpacesSpecifics& specifics =
        change->data().specifics.compass_spaces();
    mirror_[key] = specifics;
    batch->WriteData(key, specifics.SerializeAsString());
    if (specifics.has_profile()) {
      profiles_changed = true;
      continue;
    }
    (specifics.has_space() ? spaces : nodes).push_back(key);
  }

  {
    base::AutoReset<bool> applying(&applying_remote_, true);
    if (profiles_changed && delegate_) {
      // First: the home profile may take the chain's UUID, which the pass
      // below then uses.
      delegate_->OnChainProfiles(MirrorProfiles());
    }
    const std::set<std::string> created = ApplySpaces(spaces, deleted);
    // Nodes that waited for a space that just came.
    std::set<std::string> listed(nodes.begin(), nodes.end());
    for (const auto& [key, specifics] : mirror_) {
      if (specifics.has_node() &&
          created.contains(specifics.node().space_uuid()) &&
          !listed.contains(key)) {
        nodes.push_back(key);
        listed.insert(key);
      }
    }
    if (initial_merge) {
      MergeWithChain(nodes, std::set<std::string>(spaces.begin(),
                                                  spaces.end()));
    }
    ApplyNodes(nodes, deleted);
  }
  VLOG(1) << "[compass-sync] spaces: applied " << changes.size()
          << (initial_merge ? " entities (initial merge)" : " remote changes")
          << ", " << list_->spaces().size() << " spaces";

  if (!spaces.empty() || initial_merge) {
    // The switcher places the spaces among other profiles' by position
    // (slice 5), before positions are computed from it again below.
    ReportSpacePositions(/*remote=*/true);
  }
  if (initial_merge) {
    // This device's own spaces and entries join the chain's.
    SendLocalChangesTo(*batch);
  }
  Commit(std::move(batch));
  if (!initial_merge) {
    // Fix-ups the model made (cycles, invalid placements), after this
    // update is done.
    ScheduleSendLocalChanges();
  }
  return std::nullopt;
}

std::set<std::string> CompassSpacesSyncBridge::ApplySpaces(
    const std::vector<std::string>& keys,
    const std::set<std::string>& deleted) {
  std::set<std::string> created;
  for (const std::string& key : keys) {
    const sync_pb::CompassSpace& remote = mirror_[key].space();
    const base::Uuid id = ParseKey(key);
    if (!id.is_valid()) {
      continue;
    }
    SpaceModel* space = list_->FindSpace(id);
    if (!space) {
      if (FindNode(id)) {
        continue;
      }
      // Moved here from another profile on another device: if this device
      // has it in that profile, it comes over with its entries (slice 5).
      space = delegate_ ? delegate_->AdoptSpace(id) : nullptr;
      if (space && !remote.name().empty()) {
        space->SetName(base::UTF8ToUTF16(remote.name()));
      }
      if (!space) {
        space = list_->AddSpace(list_->spaces().size(),
                                base::UTF8ToUTF16(remote.name()), id);
      }
      Observe(*space);
      created.insert(key);
    } else if (!remote.name().empty()) {
      space->SetName(base::UTF8ToUTF16(remote.name()));
    }
    const int color = remote.color();
    if (color >= 1 && color <= static_cast<int>(SpaceColor::kMaxValue) + 1) {
      space->SetColor(static_cast<SpaceColor>(color - 1));
    }
    if (remote.has_archive_after_seconds() &&
        remote.archive_after_seconds() >= 0) {
      space->SetArchiveAfter(base::Seconds(remote.archive_after_seconds()));
    }
  }
  for (const std::string& key : deleted) {
    const base::Uuid id = ParseKey(key);
    if (SpaceModel* space = id.is_valid() ? list_->FindSpace(id) : nullptr) {
      // The last space stays (and the next pass puts it back on the server).
      DeleteSpace(*space);
    }
  }
  if (!keys.empty()) {
    OrderSpaces();
  }
  return created;
}

void CompassSpacesSyncBridge::OrderSpaces() {
  std::vector<std::pair<syncer::UniquePosition, SpaceModel*>> positioned;
  std::vector<SpaceModel*> others;
  for (const auto& space : list_->spaces()) {
    auto it = mirror_.find(Key(space->id()));
    std::optional<syncer::UniquePosition> position;
    if (it != mirror_.end() && it->second.has_space() &&
        it->second.space().has_directory_position()) {
      position = ValidPosition(it->second.space().directory_position());
    }
    if (position) {
      positioned.emplace_back(*position, space.get());
    } else {
      // Not on the server yet: after the chain's.
      others.push_back(space.get());
    }
  }
  std::ranges::stable_sort(positioned, [](const auto& a, const auto& b) {
    return a.first.LessThan(b.first);
  });
  std::vector<SpaceModel*> order;
  for (const auto& [position, space] : positioned) {
    order.push_back(space);
  }
  order.insert(order.end(), others.begin(), others.end());
  for (size_t i = 0; i < order.size(); ++i) {
    if (list_->spaces()[i].get() != order[i]) {
      // It's further down: the slots before i hold order[0..i).
      list_->MoveSpace(*order[i], i);
    }
  }
}

void CompassSpacesSyncBridge::MergeWithChain(
    const std::vector<std::string>& remote_nodes,
    const std::set<std::string>& remote_spaces) {
  // Favorites: the chain's copy of a site stays, this device's goes.
  std::set<std::string> chain_urls;
  for (const std::string& key : remote_nodes) {
    const sync_pb::CompassNode& node = mirror_[key].node();
    const base::Uuid id = ParseKey(key);
    if (id.is_valid() && node.section() == kSectionFavorites &&
        node.type() == kNodeTab && !FindNode(id)) {
      chain_urls.insert(node.url());
    }
  }
  SpaceModel& favorites = list_->favorites();
  std::vector<base::Uuid> duplicates;
  for (const auto& child : favorites.pinned_root()->children()) {
    if (child->type() == Type::kTab && !mirror_.contains(Key(child->id())) &&
        chain_urls.contains(child->url().possibly_invalid_spec())) {
      duplicates.push_back(child->id());
    }
  }
  for (const base::Uuid& id : duplicates) {
    if (SidebarNode* node = favorites.FindById(id)) {
      favorites.Remove(node);
    }
  }

  // A joining device's untouched starting space ("Home", nothing pinned)
  // gives way to the chain's spaces: its Today goes to the chain's first.
  SpaceModel* first = nullptr;
  for (const auto& space : list_->spaces()) {
    if (remote_spaces.contains(Key(space->id()))) {
      first = space.get();
      break;
    }
  }
  if (!first) {
    return;
  }
  std::vector<SpaceModel*> unused;
  for (const auto& space : list_->spaces()) {
    if (!mirror_.contains(Key(space->id())) && space->name() == u"Home" &&
        space->pinned_root()->children().empty()) {
      unused.push_back(space.get());
    }
  }
  for (SpaceModel* space : unused) {
    std::vector<base::Uuid> today;
    for (const auto& child : space->today_root()->children()) {
      today.push_back(child->id());
    }
    for (const base::Uuid& id : today) {
      SidebarNode* node = space->FindById(id);
      if (node && node->parent() == space->today_root()) {
        MoveNode(node, *first, first->today_root(),
                 first->today_root()->children().size());
      }
    }
    DeleteSpace(*space);
  }
}

void CompassSpacesSyncBridge::ApplyNodes(const std::vector<std::string>& keys,
                                         const std::set<std::string>& deleted) {
  // 1. New nodes (at their section's root for now) and changed content.
  std::vector<std::string> to_place;
  for (const std::string& key : keys) {
    auto it = mirror_.find(key);
    if (it == mirror_.end() || !it->second.has_node()) {
      continue;
    }
    const sync_pb::CompassNode remote = it->second.node();
    const base::Uuid id = ParseKey(key);
    if (!id.is_valid()) {
      continue;
    }
    SidebarNode* node = FindNode(id);

    if (remote.section() == kSectionToday ||
        remote.section() == kSectionArchive) {
      // Unpinned or archived on another device. Today isn't synced yet
      // (slice 3b): the entry leaves the synced sections here too.
      if (node && IsSynced(node)) {
        const base::Uuid space_id = ParseKey(remote.space_uuid());
        SpaceModel* space =
            space_id.is_valid() ? list_->FindSpace(space_id) : nullptr;
        if (remote.section() == kSectionToday && space) {
          MoveNode(node, *space, space->today_root(),
                   space->today_root()->children().size());
        } else {
          ModelOf(node)->Remove(node);
        }
      }
      continue;
    }

    SpaceModel* target = TargetModel(remote);
    const std::optional<Type> type = TypeFromProto(remote.type());
    if (!target || !type ||
        (target == &list_->favorites() && *type != Type::kTab)) {
      // Not here (yet), or not something this version can show.
      continue;
    }
    if (!node) {
      if (list_->FindSpace(id)) {
        continue;
      }
      SidebarNode* root = target->pinned_root();
      const size_t end = root->children().size();
      const std::u16string title = base::UTF8ToUTF16(remote.title());
      switch (*type) {
        case Type::kTab:
          node = target->AddTab(root, end, GURL(remote.url()), title, id);
          break;
        case Type::kFolder:
          node = target->AddFolder(root, end, title, id);
          break;
        case Type::kSplit:
          node = target->AddSplit(root, end, id);
          break;
        case Type::kRoot:
          break;
      }
      if (!node) {
        continue;
      }
    } else if (node->type() != *type) {
      // A node never changes type.
      continue;
    }

    // Content. A live tab whose URL changed keeps its page (it isn't
    // navigated); its next navigation here commits its own URL.
    SpaceModel* model = ModelOf(node);
    const std::u16string title = base::UTF8ToUTF16(remote.title());
    if (node->type() == Type::kTab) {
      const GURL url(remote.url());
      if (node->url() != url) {
        model->SetUrl(node, url);
      }
      if (node->title() != title) {
        model->SetTitle(node, title);
      }
      model->SetCustomTitle(node, base::UTF8ToUTF16(remote.custom_title()));
    } else if (node->type() == Type::kFolder) {
      if (node->title() != title) {
        model->SetTitle(node, title);
      }
      model->SetCollapsed(node, remote.collapsed());
    }
    to_place.push_back(key);
  }

  // 2. Deletions.
  ApplyNodeDeletions(deleted);

  // 3. Placement, parents first. Parked nodes may find their parent now.
  std::set<std::string> listed(to_place.begin(), to_place.end());
  std::vector<const SidebarNode*> roots = {list_->favorites().pinned_root()};
  for (const auto& space : list_->spaces()) {
    roots.push_back(space->pinned_root());
  }
  for (const SidebarNode* root : roots) {
    for (const auto& child : root->children()) {
      const std::string key = Key(child->id());
      auto it = mirror_.find(key);
      if (it != mirror_.end() && it->second.has_node() &&
          !it->second.node().parent_uuid().empty() && !listed.contains(key)) {
        to_place.push_back(key);
        listed.insert(key);
      }
    }
  }
  std::map<std::string, int> depth;
  for (const std::string& key : to_place) {
    int d = 0;
    std::set<std::string> seen = {key};
    for (auto it = mirror_.find(key);
         it != mirror_.end() && it->second.has_node() &&
         !it->second.node().parent_uuid().empty() &&
         seen.insert(it->second.node().parent_uuid()).second;
         it = mirror_.find(it->second.node().parent_uuid())) {
      ++d;
    }
    depth[key] = d;
  }
  std::ranges::stable_sort(to_place, [&depth](const std::string& a,
                                              const std::string& b) {
    return depth[a] < depth[b];
  });
  for (const std::string& key : to_place) {
    PlaceNode(key, deleted);
  }

  // 4. Splits the batch left with fewer than two panes.
  NormalizeSplits();
}

void CompassSpacesSyncBridge::ApplyNodeDeletions(
    const std::set<std::string>& deleted) {
  // Entries added here to a folder deleted elsewhere aren't lost: they go to
  // the section's root (and the next pass commits that).
  for (const std::string& key : deleted) {
    const base::Uuid id = ParseKey(key);
    SidebarNode* node = id.is_valid() ? FindNode(id) : nullptr;
    if (!node || !IsSynced(node)) {
      continue;
    }
    SpaceModel* model = ModelOf(node);
    std::vector<base::Uuid> kept;
    for (const auto& child : node->children()) {
      if (!deleted.contains(Key(child->id()))) {
        kept.push_back(child->id());
      }
    }
    for (const base::Uuid& child_id : kept) {
      SidebarNode* child = model->FindById(child_id);
      SidebarNode* root = model->pinned_root();
      if (child && model->CanMove(child, root)) {
        model->Move(child, root, root->children().size());
      }
    }
  }
  // Moving panes out can remove their split, so go by ID. Removing an entry
  // closes its live tabs, as a local delete does.
  for (const std::string& key : deleted) {
    const base::Uuid id = ParseKey(key);
    SidebarNode* node = id.is_valid() ? FindNode(id) : nullptr;
    if (node && IsSynced(node)) {
      ModelOf(node)->Remove(node);
    }
  }
}

void CompassSpacesSyncBridge::PlaceNode(const std::string& key,
                                        const std::set<std::string>& deleted) {
  auto it = mirror_.find(key);
  if (it == mirror_.end() || !it->second.has_node()) {
    return;
  }
  const sync_pb::CompassNode remote = it->second.node();
  SidebarNode* node = FindNode(ParseKey(key));
  SpaceModel* target = TargetModel(remote);
  if (!node || !target) {
    return;
  }
  SidebarNode* root = target->pinned_root();
  SidebarNode* parent = root;
  // Whether it goes (or stays) at the end of the root rather than where its
  // position says: that position is among its parent's children, not the
  // root's.
  bool at_root_end = false;
  if (!remote.parent_uuid().empty()) {
    const base::Uuid parent_id = ParseKey(remote.parent_uuid());
    SidebarNode* wanted = parent_id.is_valid() ? FindNode(parent_id) : nullptr;
    if (deleted.contains(remote.parent_uuid())) {
      // Its parent is gone: it stays at the end of the root, where
      // ApplyNodeDeletions() put it, and the next pass commits that.
      unparked_.insert(key);
      at_root_end = true;
    } else if (wanted) {
      SpaceModel* source = ModelOf(node);
      // A cycle (the parent is inside it) or an invalid placement (a folder
      // in a split) leaves it at the root; the next pass commits that.
      const bool valid =
          root->Contains(wanted) && !node->Contains(wanted) &&
          (source == target ? target->CanMove(node, wanted)
                            : source->CanMoveToSpace(node, *target, wanted));
      if (valid) {
        parent = wanted;
      }
    } else {
      // Its parent hasn't come yet: it waits at the end of the root,
      // parked.
      at_root_end = true;
    }
  }
  if (at_root_end && node->parent() == root) {
    return;
  }
  const size_t index = at_root_end
                           ? root->children().size()
                           : InsertionIndex(*parent, node, remote.position());
  MoveNode(node, *target, parent, index);
}

void CompassSpacesSyncBridge::NormalizeSplits() {
  std::vector<base::Uuid> splits;
  for (const auto& space : list_->spaces()) {
    CollectSplits(*space->pinned_root(), splits);
  }
  for (const base::Uuid& id : splits) {
    SidebarNode* split = FindNode(id);
    if (!split || split->type() != Type::kSplit) {
      continue;
    }
    SpaceModel* model = ModelOf(split);
    if (split->children().empty()) {
      model->Remove(split);
    } else if (split->children().size() == 1) {
      model->Unsplit(split);
    }
  }
}

// Model helpers ---------------------------------------------------------------

SidebarNode* CompassSpacesSyncBridge::FindNode(const base::Uuid& id) const {
  if (SidebarNode* node = list_->favorites().FindById(id)) {
    return node;
  }
  for (const auto& space : list_->spaces()) {
    if (SidebarNode* node = space->FindById(id)) {
      return node;
    }
  }
  return nullptr;
}

SpaceModel* CompassSpacesSyncBridge::ModelOf(const SidebarNode* node) const {
  if (list_->favorites().Contains(node)) {
    return &list_->favorites();
  }
  return list_->FindSpaceOf(node);
}

bool CompassSpacesSyncBridge::IsSynced(const SidebarNode* node) const {
  if (list_->favorites().pinned_root()->Contains(node)) {
    return true;
  }
  const SpaceModel* space = list_->FindSpaceOf(node);
  return space && space->pinned_root()->Contains(node);
}

SpaceModel* CompassSpacesSyncBridge::TargetModel(
    const sync_pb::CompassNode& node) const {
  switch (node.section()) {
    case kSectionFavorites:
      return &list_->favorites();
    case kSectionPinned: {
      const base::Uuid id = ParseKey(node.space_uuid());
      return id.is_valid() ? list_->FindSpace(id) : nullptr;
    }
    default:
      return nullptr;
  }
}

void CompassSpacesSyncBridge::MoveNode(SidebarNode* node,
                                       SpaceModel& target,
                                       SidebarNode* parent,
                                       size_t index) {
  SpaceModel* source = ModelOf(node);
  if (!source) {
    return;
  }
  if (source == &target) {
    if (node->parent() == parent && parent->GetIndexOf(node) == index) {
      return;
    }
    if (!target.CanMove(node, parent)) {
      return;
    }
    // |index| leaves |node| out.
    const size_t others =
        parent->children().size() - (node->parent() == parent ? 1 : 0);
    target.Move(node, parent, std::min(index, others));
    return;
  }
  if (!source->CanMoveToSpace(node, target, parent)) {
    return;
  }
  index = std::min(index, parent->children().size());
  if (delegate_) {
    delegate_->MoveEntry(node, target, parent, index);
  } else {
    source->MoveToSpace(node, target, parent, index);
  }
}

void CompassSpacesSyncBridge::DeleteSpace(SpaceModel& space) {
  if (delegate_) {
    delegate_->DeleteSpace(space);
  } else if (list_->spaces().size() > 1) {
    list_->RemoveSpace(space);
  }
}

size_t CompassSpacesSyncBridge::InsertionIndex(
    const SidebarNode& parent,
    const SidebarNode* node,
    const sync_pb::UniquePosition& position) const {
  const std::optional<syncer::UniquePosition> target = ValidPosition(position);
  size_t index = 0;
  for (const auto& child : parent.children()) {
    if (child.get() == node) {
      continue;
    }
    if (target) {
      // Before the first sibling the server has after it, or the first one
      // it doesn't have there (this device's own, not sent yet, or parked):
      // after an initial merge, the chain's entries come first.
      std::optional<syncer::UniquePosition> sibling =
          MirrorPosition(*child, parent);
      if (!sibling || target->LessThan(*sibling)) {
        return index;
      }
    }
    ++index;
  }
  return index;
}

std::optional<syncer::UniquePosition> CompassSpacesSyncBridge::MirrorPosition(
    const SidebarNode& child,
    const SidebarNode& parent) const {
  auto it = mirror_.find(Key(child.id()));
  if (it == mirror_.end() || !it->second.has_node() ||
      it->second.node().parent_uuid() != ParentKey(parent) ||
      !it->second.node().has_position()) {
    return std::nullopt;
  }
  return ValidPosition(it->second.node().position());
}

std::string CompassSpacesSyncBridge::ParentKey(
    const SidebarNode& parent) const {
  return parent.type() == Type::kRoot ? std::string() : Key(parent.id());
}

void CompassSpacesSyncBridge::Observe(SpaceModel& model) {
  // Removes first, so a model already observed isn't observed twice.
  model.RemoveObserver(this);
  model.AddObserver(this);
}

// The rest of DataTypeSyncBridge ----------------------------------------------

std::unique_ptr<syncer::DataBatch> CompassSpacesSyncBridge::GetDataForCommit(
    StorageKeyList storage_keys) {
  auto batch = std::make_unique<syncer::MutableDataBatch>();
  for (const std::string& key : storage_keys) {
    auto it = mirror_.find(key);
    if (it == mirror_.end()) {
      continue;
    }
    auto data = std::make_unique<syncer::EntityData>();
    data->name = key;
    *data->specifics.mutable_compass_spaces() = it->second;
    batch->Put(key, std::move(data));
  }
  return batch;
}

std::unique_ptr<syncer::DataBatch>
CompassSpacesSyncBridge::GetAllDataForDebugging() {
  StorageKeyList keys;
  for (const auto& [key, specifics] : mirror_) {
    keys.push_back(key);
  }
  return GetDataForCommit(std::move(keys));
}

std::string CompassSpacesSyncBridge::GetClientTag(
    const syncer::EntityData& entity_data) const {
  return GetStorageKey(entity_data);
}

std::string CompassSpacesSyncBridge::GetStorageKey(
    const syncer::EntityData& entity_data) const {
  const sync_pb::CompassSpacesSpecifics& specifics =
      entity_data.specifics.compass_spaces();
  if (specifics.has_space()) {
    return specifics.space().uuid();
  }
  if (specifics.has_profile()) {
    return specifics.profile().uuid();
  }
  return specifics.node().uuid();
}

void CompassSpacesSyncBridge::ApplyDisableSyncChanges(
    std::unique_ptr<syncer::MetadataChangeList> delete_metadata_change_list) {
  // Leaving the chain keeps the spaces; only what sync knew goes.
  mirror_.clear();
  unparked_.clear();
  if (store_) {
    store_->DeleteAllDataAndMetadata(std::move(delete_metadata_change_list),
                                     base::DoNothing());
  }
}

sync_pb::EntitySpecifics
CompassSpacesSyncBridge::TrimAllSupportedFieldsFromRemoteSpecifics(
    const sync_pb::EntitySpecifics& entity_specifics) const {
  // The mirror keeps every field, known or not, so the processor needn't.
  return sync_pb::EntitySpecifics();
}

bool CompassSpacesSyncBridge::IsEntityDataValid(
    const syncer::EntityData& entity_data) const {
  if (!entity_data.specifics.has_compass_spaces()) {
    return false;
  }
  const sync_pb::CompassSpacesSpecifics& specifics =
      entity_data.specifics.compass_spaces();
  if (!specifics.has_space() && !specifics.has_node() &&
      !specifics.has_profile()) {
    return false;
  }
  return ParseKey(GetStorageKey(entity_data)).is_valid();
}

// Observers -------------------------------------------------------------------

void CompassSpacesSyncBridge::OnSpaceAdded(SpaceModel& space) {
  Observe(space);
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnSpaceUpdated(const SpaceModel& space) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnSpaceWillBeRemoved(SpaceModel& space) {
  space.RemoveObserver(this);
}

void CompassSpacesSyncBridge::OnSpaceRemoved() {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnSpaceMoved(const SpaceModel& space) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnNodeAdded(const SidebarNode& node) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnNodeMoved(const SidebarNode& node,
                                          const SidebarNode& old_parent,
                                          size_t old_index) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnNodeChanged(const SidebarNode& node) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnNodeWillBeRemoved(const SidebarNode& node) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnNodeWillMoveToSpace(const SidebarNode& node) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

#undef COMPASS_SET_STRING

}  // namespace compass::sync
