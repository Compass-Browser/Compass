// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/rooms/room_model.h"

#include <algorithm>
#include <utility>

#include "base/memory/raw_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/sync/compass_spaces_specifics_util.h"
#include "compass/browser/sync/compass_spaces_sync_bridge.h"
#include "url/gurl.h"

namespace compass::rooms {

namespace {

using Type = SidebarNode::Type;
namespace util = sync::spaces_util;

std::string Key(const base::Uuid& id) {
  return id.AsLowercaseString();
}

base::Uuid ParseKey(const std::string& key) {
  return base::Uuid::ParseLowercase(key);
}

// Finds |id| among |list|'s spaces (their Pinned and Today trees).
SidebarNode* FindInList(SpaceList& list, const base::Uuid& id) {
  for (const auto& space : list.spaces()) {
    if (SidebarNode* node = space->FindById(id)) {
      return node;
    }
  }
  return nullptr;
}

void CollectEasels(const SidebarNode& parent, std::set<std::string>& out) {
  for (const auto& child : parent.children()) {
    if (child->type() == Type::kTab) {
      if (std::string easel = EaselOfTab(*child); !easel.empty()) {
        out.insert(easel);
      }
    }
    CollectEasels(*child, out);
  }
}

// The records of |parent|'s children and their subtrees, as the bridge's
// AddLocalNodes() makes them: |parent_key| is the parent's UUID ("" for a
// space's Pinned root), |space_key| the space's ("" in a folder's room, where
// each member has the folder in a space of their own).
void AddTree(const SidebarNode& parent,
             const std::string& parent_key,
             const std::string& space_key,
             const Records& mirror,
             const std::set<std::string>& excluded,
             Records& out) {
  std::vector<const SidebarNode*> placed;
  std::vector<std::string> keys;
  std::vector<std::optional<syncer::UniquePosition>> existing;
  for (const auto& child : parent.children()) {
    if (child->is_live_item()) {
      // Made on each device (docs/design/live-folders.md, "Sync").
      continue;
    }
    const std::string key = Key(child->id());
    if (out.contains(key) || excluded.contains(key)) {
      continue;
    }
    std::optional<syncer::UniquePosition> position;
    auto it = mirror.find(key);
    if (it != mirror.end() && it->second.has_node() &&
        it->second.node().parent_uuid() == parent_key &&
        it->second.node().has_position()) {
      position = util::ValidPosition(it->second.node().position());
    }
    out[key];
    placed.push_back(child.get());
    keys.push_back(key);
    existing.push_back(position);
  }
  const std::vector<syncer::UniquePosition> positions =
      sync::internal::AssignPositions(keys, existing);
  for (size_t i = 0; i < placed.size(); ++i) {
    sync_pb::CompassSpacesSpecifics specifics;
    auto it = mirror.find(keys[i]);
    if (it != mirror.end() && it->second.has_node()) {
      specifics = it->second;
    }
    sync_pb::CompassNode& node = *specifics.mutable_node();
    node.set_uuid(keys[i]);
    if (space_key.empty()) {
      node.clear_space_uuid();
    } else {
      node.set_space_uuid(space_key);
    }
    if (parent_key.empty()) {
      node.clear_parent_uuid();
    } else {
      node.set_parent_uuid(parent_key);
    }
    node.set_section(sync::kSectionPinned);
    node.set_type(util::NodeTypeToProto(placed[i]->type()));
    util::FillNodeContent(*placed[i], node);
    // Personal, as Today's: never shared.
    node.clear_last_active_time();
    node.clear_archived_time();
    if (!existing[i] || !existing[i]->Equals(positions[i])) {
      *node.mutable_position() = positions[i].ToProto();
    }
    out[keys[i]] = std::move(specifics);
  }
  for (const SidebarNode* child : placed) {
    if (!child->children().empty()) {
      AddTree(*child, Key(child->id()), space_key, mirror, excluded, out);
    }
  }
}

// Where a node at |position| goes among |parent|'s children (|node| left
// out), by the positions the mirror has for them.
size_t InsertionIndex(const SidebarNode& parent,
                      const std::string& parent_key,
                      const SidebarNode* node,
                      const sync_pb::UniquePosition& position,
                      const Records& mirror) {
  const std::optional<syncer::UniquePosition> target =
      util::ValidPosition(position);
  size_t index = 0;
  for (const auto& child : parent.children()) {
    if (child.get() == node) {
      continue;
    }
    if (target) {
      auto it = mirror.find(Key(child->id()));
      std::optional<syncer::UniquePosition> sibling;
      if (it != mirror.end() && it->second.has_node() &&
          it->second.node().parent_uuid() == parent_key &&
          it->second.node().has_position()) {
        sibling = util::ValidPosition(it->second.node().position());
      }
      // Siblings the room doesn't have (this device's, about to go or
      // to be written) don't count.
      if (sibling && target->LessThan(*sibling)) {
        return index;
      }
    }
    ++index;
  }
  return index;
}

// Applies the room's nodes under |container| (a shared folder, or a shared
// space's Pinned root, whose key is |container_key|).
void ApplyTree(SpaceModel& model,
               SidebarNode* container,
               const std::string& container_key,
               const Records& mirror,
               const std::set<std::string>& changed,
               const Records& deleted,
               const std::set<std::string>& excluded) {
  SpaceModel::KeepEmptyFolders keep_empty_folders;
  // 1. New nodes (in the container for now) and content.
  std::vector<std::string> to_place;
  for (const std::string& key : changed) {
    auto it = mirror.find(key);
    if (key == container_key || excluded.contains(key) ||
        it == mirror.end() || !it->second.has_node()) {
      continue;
    }
    const sync_pb::CompassNode& remote = it->second.node();
    const std::optional<Type> type = util::NodeTypeFromProto(remote.type());
    const base::Uuid id = ParseKey(key);
    if (!type || !id.is_valid()) {
      continue;
    }
    SidebarNode* node = model.FindById(id);
    if (node && !container->Contains(node)) {
      // Here, but outside the shared item: left alone.
      continue;
    }
    if (!node) {
      const size_t end = container->children().size();
      const std::u16string title = base::UTF8ToUTF16(remote.title());
      switch (*type) {
        case Type::kTab:
          node = model.AddTab(container, end, GURL(remote.url()), title, id);
          break;
        case Type::kFolder:
          if (std::optional<LiveSource> live = util::LiveSourceFromProto(remote);
              live && model.CanAddLiveFolder(container)) {
            node = model.AddLiveFolder(container, end, title, std::move(*live),
                                       id);
          } else {
            node = model.AddFolder(container, end, title, id);
          }
          break;
        case Type::kSplit:
          node = model.AddSplit(container, end, id);
          break;
        case Type::kRoot:
          break;
      }
      if (!node) {
        continue;
      }
      if (node->type() == Type::kTab && remote.has_saved_url()) {
        const GURL saved(remote.saved_url());
        if (saved.is_valid()) {
          model.SetSavedUrl(node, saved,
                            base::UTF8ToUTF16(remote.saved_title()));
        }
      }
    } else if (node->type() != *type) {
      continue;
    }
    util::ApplyNodeContent(model, node, remote, /*keep_page=*/false);
    to_place.push_back(key);
  }

  // 2. Placement, parents first.
  std::map<std::string, int> depth;
  for (const std::string& key : to_place) {
    int d = 0;
    std::set<std::string> seen = {key};
    for (auto it = mirror.find(key);
         it != mirror.end() && it->second.has_node() &&
         !it->second.node().parent_uuid().empty() &&
         it->second.node().parent_uuid() != container_key &&
         seen.insert(it->second.node().parent_uuid()).second;
         it = mirror.find(it->second.node().parent_uuid())) {
      ++d;
    }
    depth[key] = d;
  }
  std::ranges::stable_sort(to_place, [&depth](const std::string& a,
                                              const std::string& b) {
    return depth[a] < depth[b];
  });
  for (const std::string& key : to_place) {
    const sync_pb::CompassNode& remote = mirror.at(key).node();
    SidebarNode* node = model.FindById(ParseKey(key));
    if (!node) {
      continue;
    }
    SidebarNode* parent = container;
    std::string parent_key = container_key;
    if (!remote.parent_uuid().empty() && remote.parent_uuid() != container_key) {
      SidebarNode* wanted = model.FindById(ParseKey(remote.parent_uuid()));
      if (wanted && container->Contains(wanted) && wanted != container &&
          !node->Contains(wanted) && model.CanMove(node, wanted)) {
        parent = wanted;
        parent_key = remote.parent_uuid();
      }
    }
    if (parent == container && !model.CanMove(node, container) &&
        node->parent() != container) {
      continue;
    }
    const size_t index =
        InsertionIndex(*parent, parent_key, node, remote.position(), mirror);
    if (node->parent() == parent && parent->GetIndexOf(node) == index) {
      continue;
    }
    model.Move(node, parent, index);
  }

  // 3. Deletions, after moves (a node moved out of a deleted folder stays).
  for (const auto& [key, specifics] : deleted) {
    if (!specifics.has_node() || key == container_key ||
        excluded.contains(key)) {
      continue;
    }
    SidebarNode* node = model.FindById(ParseKey(key));
    if (node && node != container && container->Contains(node)) {
      model.Remove(node);
    }
  }

  // 4. Splits left with fewer than two panes.
  std::vector<base::Uuid> splits;
  std::vector<const SidebarNode*> stack = {container};
  while (!stack.empty()) {
    const SidebarNode* n = stack.back();
    stack.pop_back();
    for (const auto& child : n->children()) {
      if (child->type() == Type::kSplit) {
        splits.push_back(child->id());
      }
      stack.push_back(child.get());
    }
  }
  for (const base::Uuid& id : splits) {
    SidebarNode* split = model.FindById(id);
    if (!split || split->type() != Type::kSplit) {
      continue;
    }
    if (split->children().empty()) {
      model.Remove(split);
    } else if (split->children().size() == 1) {
      model.Unsplit(split);
    }
  }
}

// Whether |node| is in (or is) one of |excluded| below |container|.
bool InExcludedOf(const SidebarNode& node,
                  const SidebarNode* container,
                  const std::set<std::string>& excluded) {
  for (const SidebarNode* n = &node; n && n != container; n = n->parent()) {
    if (excluded.contains(Key(n->id()))) {
      return true;
    }
  }
  return false;
}

bool IsEaselEntity(const sync_pb::CompassSpacesSpecifics& specifics) {
  return specifics.has_easel() || specifics.has_easel_object() ||
         specifics.has_easel_asset();
}

// The records of |easels|' boards, objects and images.
std::optional<Records> EaselRecords(const EaselSource* source,
                                    const std::set<std::string>& easels,
                                    const Records& mirror) {
  if (easels.empty()) {
    return Records();
  }
  if (!source) {
    return std::nullopt;
  }
  std::optional<std::vector<sync_pb::CompassSpacesSpecifics>> entities =
      source->Entities();
  if (!entities) {
    return std::nullopt;
  }
  Records out;
  std::set<std::string> assets;
  for (const sync_pb::CompassSpacesSpecifics& entity : *entities) {
    if (entity.has_easel() && easels.contains(entity.easel().uuid())) {
      out[entity.easel().uuid()] = entity;
    } else if (entity.has_easel_object() &&
               easels.contains(entity.easel_object().easel_uuid())) {
      out[entity.easel_object().uuid()] = entity;
      for (const std::string& sha256 :
           util::EaselObjectAssets(entity.easel_object())) {
        assets.insert(sha256);
      }
    }
  }
  for (const sync_pb::CompassSpacesSpecifics& entity : *entities) {
    if (!entity.has_easel_asset() ||
        !assets.contains(entity.easel_asset().sha256())) {
      continue;
    }
    const std::string& key = entity.easel_asset().uuid();
    // An image never changes (it's named by its hash): the room's copy.
    if (auto it = mirror.find(key);
        it != mirror.end() && it->second.has_easel_asset()) {
      out[key] = it->second;
      continue;
    }
    std::optional<std::string> data =
        source->AssetData(entity.easel_asset().sha256());
    if (!data) {
      continue;
    }
    sync_pb::CompassSpacesSpecifics& record = out[key];
    record = entity;
    record.mutable_easel_asset()->set_data(std::move(*data));
  }
  return out;
}

void ApplyEasels(EaselSource* source,
                 const Records& mirror,
                 const std::set<std::string>& changed,
                 const Records& deleted) {
  if (!source) {
    return;
  }
  std::vector<sync_pb::CompassSpacesSpecifics> easels;
  std::set<std::string> gone;
  for (const std::string& key : changed) {
    auto it = mirror.find(key);
    if (it != mirror.end() && IsEaselEntity(it->second)) {
      easels.push_back(it->second);
    }
  }
  for (const auto& [key, specifics] : deleted) {
    if (IsEaselEntity(specifics)) {
      gone.insert(key);
    }
  }
  if (!easels.empty() || !gone.empty()) {
    source->Apply(easels, gone);
  }
}

// Where |placement| puts a new folder or tab in |list|: the parent and the
// index (by position among siblings that have none: at the end).
std::pair<SpaceModel*, SidebarNode*> ResolvePlacement(
    SpaceList& list,
    const Placement& placement) {
  SpaceModel* space = nullptr;
  if (base::Uuid id = ParseKey(placement.space_uuid); id.is_valid()) {
    space = list.FindSpace(id);
  }
  if (!space) {
    // As iOS: the account's first space.
    space = list.spaces().front().get();
  }
  SidebarNode* parent = space->pinned_root();
  if (base::Uuid id = ParseKey(placement.parent_uuid); id.is_valid()) {
    if (SidebarNode* wanted = space->FindById(id);
        wanted && wanted->type() == Type::kFolder &&
        space->pinned_root()->Contains(wanted)) {
      parent = wanted;
    }
  }
  return {space, parent};
}

class FolderRoomModel : public RoomModel {
 public:
  FolderRoomModel(SpaceList* list,
                  EaselSource* easels,
                  std::string root_uuid,
                  base::RepeatingCallback<Placement()> placement)
      : list_(list),
        easels_(easels),
        root_uuid_(std::move(root_uuid)),
        placement_(std::move(placement)) {}

  bool HasRoot() const override { return Root(); }

  std::optional<Records> GetLocal(const Records& mirror) const override {
    SidebarNode* root = Root();
    if (!root) {
      return std::nullopt;
    }
    Records out;
    sync_pb::CompassSpacesSpecifics specifics;
    if (auto it = mirror.find(root_uuid_);
        it != mirror.end() && it->second.has_node()) {
      specifics = it->second;
    }
    sync_pb::CompassNode& node = *specifics.mutable_node();
    node.set_uuid(root_uuid_);
    node.set_type(sync::kNodeFolder);
    node.set_section(sync::kSectionPinned);
    // Where it is is each member's own (their mount's).
    node.clear_space_uuid();
    node.clear_parent_uuid();
    node.clear_position();
    util::FillNodeContent(*root, node);
    out[root_uuid_] = std::move(specifics);
    AddTree(*root, root_uuid_, std::string(), mirror, excluded_, out);
    std::optional<Records> easels =
        EaselRecords(easels_, GetEasels(), mirror);
    if (!easels) {
      return std::nullopt;
    }
    out.merge(*easels);
    return out;
  }

  void Apply(const Records& mirror,
             const std::set<std::string>& changed,
             const Records& deleted) override {
    SidebarNode* root = Root();
    auto it = mirror.find(root_uuid_);
    if (!root) {
      if (it == mirror.end() || !it->second.has_node()) {
        // The folder's own record hasn't come yet.
        return;
      }
      const auto [space, parent] = ResolvePlacement(*list_, placement_.Run());
      root = space->AddFolder(parent, parent->children().size(),
                              base::UTF8ToUTF16(it->second.node().title()),
                              ParseKey(root_uuid_));
      if (!root) {
        return;
      }
      // Everything, now that the folder is here.
      std::set<std::string> all;
      for (const auto& [key, specifics] : mirror) {
        all.insert(key);
      }
      ApplyAll(root, mirror, all, deleted);
      return;
    }
    ApplyAll(root, mirror, changed, deleted);
  }

  std::set<std::string> GetEasels() const override {
    std::set<std::string> easels;
    if (SidebarNode* root = Root()) {
      CollectEasels(*root, easels);
    }
    return easels;
  }

  bool Contains(const SidebarNode& node) const override {
    SidebarNode* root = Root();
    return root && root->Contains(&node) && !InExcludedOf(node, root, excluded_);
  }

 private:
  SidebarNode* Root() const {
    SidebarNode* node = FindInList(*list_, ParseKey(root_uuid_));
    return node && node->type() == Type::kFolder ? node : nullptr;
  }

  void ApplyAll(SidebarNode* root,
                const Records& mirror,
                const std::set<std::string>& changed,
                const Records& deleted) {
    SpaceModel* model = list_->FindSpaceOf(root);
    if (!model) {
      return;
    }
    if (changed.contains(root_uuid_)) {
      util::ApplyNodeContent(*model, root, mirror.at(root_uuid_).node(),
                             /*keep_page=*/false);
    }
    ApplyTree(*model, root, root_uuid_, mirror, changed, deleted, excluded_);
    ApplyEasels(easels_, mirror, changed, deleted);
  }

  const raw_ptr<SpaceList> list_;
  const raw_ptr<EaselSource> easels_;
  const std::string root_uuid_;
  base::RepeatingCallback<Placement()> placement_;
};

class SpaceRoomModel : public RoomModel {
 public:
  SpaceRoomModel(SpaceList* list, EaselSource* easels, std::string root_uuid)
      : list_(list), easels_(easels), root_uuid_(std::move(root_uuid)) {}

  bool HasRoot() const override { return Space(); }

  std::optional<Records> GetLocal(const Records& mirror) const override {
    SpaceModel* space = Space();
    if (!space) {
      return std::nullopt;
    }
    Records out;
    sync_pb::CompassSpacesSpecifics specifics;
    if (auto it = mirror.find(root_uuid_);
        it != mirror.end() && it->second.has_space()) {
      specifics = it->second;
    }
    sync_pb::CompassSpace& entity = *specifics.mutable_space();
    entity.set_uuid(root_uuid_);
    entity.set_name(base::UTF16ToUTF8(space->name()));
    util::ThemeToProto(*space, entity);
    // The switcher's order and the profile are each member's own; so is
    // the archive delay (Today is personal).
    entity.clear_directory_position();
    entity.clear_profile_uuid();
    entity.clear_archive_after_seconds();
    out[root_uuid_] = std::move(specifics);
    AddTree(*space->pinned_root(), std::string(), root_uuid_, mirror, excluded_,
            out);
    std::optional<Records> easels = EaselRecords(easels_, GetEasels(), mirror);
    if (!easels) {
      return std::nullopt;
    }
    out.merge(*easels);
    return out;
  }

  void Apply(const Records& mirror,
             const std::set<std::string>& changed,
             const Records& deleted) override {
    SpaceModel* space = Space();
    auto it = mirror.find(root_uuid_);
    std::set<std::string> all;
    const std::set<std::string>* keys = &changed;
    if (!space) {
      if (it == mirror.end() || !it->second.has_space()) {
        return;
      }
      space = list_->AddSpace(list_->spaces().size(),
                              base::UTF8ToUTF16(it->second.space().name()),
                              ParseKey(root_uuid_));
      if (!space) {
        return;
      }
      for (const auto& [key, specifics] : mirror) {
        all.insert(key);
      }
      keys = &all;
    }
    if (keys->contains(root_uuid_) && it != mirror.end() &&
        it->second.has_space()) {
      const sync_pb::CompassSpace& remote = it->second.space();
      if (!remote.name().empty()) {
        space->SetName(base::UTF8ToUTF16(remote.name()));
      }
      util::ThemeFromProto(remote, *space);
    }
    ApplyTree(*space, space->pinned_root(), std::string(), mirror, *keys,
              deleted, excluded_);
    ApplyEasels(easels_, mirror, *keys, deleted);
  }

  std::set<std::string> GetEasels() const override {
    std::set<std::string> easels;
    if (SpaceModel* space = Space()) {
      CollectEasels(*space->pinned_root(), easels);
    }
    return easels;
  }

  bool Contains(const SidebarNode& node) const override {
    SpaceModel* space = Space();
    return space && space->pinned_root()->Contains(&node) &&
           !InExcludedOf(node, space->pinned_root(), excluded_);
  }

 private:
  SpaceModel* Space() const {
    const base::Uuid id = ParseKey(root_uuid_);
    return id.is_valid() ? list_->FindSpace(id) : nullptr;
  }

  const raw_ptr<SpaceList> list_;
  const raw_ptr<EaselSource> easels_;
  const std::string root_uuid_;
};

class EaselRoomModel : public RoomModel {
 public:
  EaselRoomModel(SpaceList* list,
                 EaselSource* easels,
                 std::string root_uuid,
                 std::string tab_uuid,
                 base::RepeatingCallback<Placement()> placement)
      : list_(list),
        easels_(easels),
        root_uuid_(std::move(root_uuid)),
        tab_uuid_(std::move(tab_uuid)),
        placement_(std::move(placement)) {}

  bool HasRoot() const override {
    if (!easels_) {
      return false;
    }
    std::optional<std::vector<sync_pb::CompassSpacesSpecifics>> entities =
        easels_->Entities();
    return entities &&
           std::ranges::any_of(*entities, [this](const auto& entity) {
             return entity.has_easel() && entity.easel().uuid() == root_uuid_;
           });
  }

  std::optional<Records> GetLocal(const Records& mirror) const override {
    if (!HasRoot()) {
      return std::nullopt;
    }
    return EaselRecords(easels_, {root_uuid_}, mirror);
  }

  void Apply(const Records& mirror,
             const std::set<std::string>& changed,
             const Records& deleted) override {
    const bool had = HasRoot();
    ApplyEasels(easels_, mirror, changed, deleted);
    if (had || !mirror.contains(root_uuid_) || !list_) {
      return;
    }
    // The board just came: a pinned tab for it, unless one is here.
    const std::string url = EaselTabUrl(root_uuid_);
    for (const auto& space : list_->spaces()) {
      std::set<std::string> found;
      CollectEasels(*space->pinned_root(), found);
      if (found.contains(root_uuid_)) {
        return;
      }
    }
    const auto [space, parent] = ResolvePlacement(*list_, placement_.Run());
    space->AddTab(parent, parent->children().size(), GURL(url),
                  base::UTF8ToUTF16(mirror.at(root_uuid_).easel().title()),
                  base::Uuid::ParseLowercase(tab_uuid_));
  }

  std::set<std::string> GetEasels() const override { return {root_uuid_}; }

  bool Contains(const SidebarNode& node) const override {
    return EaselOfTab(node) == root_uuid_;
  }

 private:
  const raw_ptr<SpaceList> list_;
  const raw_ptr<EaselSource> easels_;
  const std::string root_uuid_;
  const std::string tab_uuid_;
  base::RepeatingCallback<Placement()> placement_;
};

}  // namespace

std::string EaselTabUrl(const std::string& easel_uuid) {
  return easels::EaselUrl(easel_uuid);
}

std::string EaselOfTab(const SidebarNode& node) {
  if (node.type() != Type::kTab) {
    return std::string();
  }
  std::string easel = easels::EaselUuidFromUrl(node.url().spec());
  if (easel.empty() && node.saved_url().is_valid()) {
    easel = easels::EaselUuidFromUrl(node.saved_url().spec());
  }
  return easel;
}

RoomModel::RoomModel() = default;
RoomModel::~RoomModel() = default;

Placement::Placement() = default;
Placement::Placement(const Placement&) = default;
Placement& Placement::operator=(const Placement&) = default;
Placement::~Placement() = default;

std::unique_ptr<RoomModel> CreateFolderRoomModel(
    SpaceList* list,
    EaselSource* easels,
    std::string root_uuid,
    base::RepeatingCallback<Placement()> placement) {
  return std::make_unique<FolderRoomModel>(list, easels, std::move(root_uuid),
                                           std::move(placement));
}

std::unique_ptr<RoomModel> CreateSpaceRoomModel(SpaceList* list,
                                                EaselSource* easels,
                                                std::string root_uuid) {
  return std::make_unique<SpaceRoomModel>(list, easels, std::move(root_uuid));
}

std::unique_ptr<RoomModel> CreateEaselRoomModel(
    SpaceList* list,
    EaselSource* easels,
    std::string root_uuid,
    std::string tab_uuid,
    base::RepeatingCallback<Placement()> placement) {
  return std::make_unique<EaselRoomModel>(list, easels, std::move(root_uuid),
                                          std::move(tab_uuid),
                                          std::move(placement));
}

}  // namespace compass::rooms
