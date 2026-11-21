// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ROOMS_ROOM_MODEL_H_
#define COMPASS_BROWSER_ROOMS_ROOM_MODEL_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"
#include "components/sync/base/unique_position.h"

namespace compass {
class SidebarNode;
class SpaceList;
class SpaceModel;
}  // namespace compass

// The model side of a room (docs/design/sync.md, "Rooms: clients"): the
// shared item as records (CompassSpacesSpecifics keyed by record ID, the
// entity's UUID), and applying the room's records to it. Scoped to the shared
// subtree: nothing outside it is read or changed. Shared by the desktop and
// Android, which both keep their spaces in a SpaceList.
namespace compass::rooms {

using Records = std::map<std::string, sync_pb::CompassSpacesSpecifics>;

// An easel tab's URL ("compass://easel/<uuid>") and the board of one.
// Exposed for tests.
std::string EaselTabUrl(const std::string& easel_uuid);
std::string EaselOfTab(const SidebarNode& node);

// The profile's Easels as entities (EaselStore through easel_sync.h; a fake
// in tests).
class EaselSource {
 public:
  virtual ~EaselSource() = default;
  // Every board, object and image (without bytes); nullopt until loaded.
  virtual std::optional<std::vector<sync_pb::CompassSpacesSpecifics>>
  Entities() const = 0;
  virtual std::optional<std::string> AssetData(
      const std::string& sha256) const = 0;
  // Boards, objects and images of a room that changed, and the keys of those
  // it deleted.
  virtual void Apply(
      const std::vector<sync_pb::CompassSpacesSpecifics>& changed,
      const std::set<std::string>& deleted) = 0;
};

// Where a member puts a shared folder or easel tab (its mount's placement).
struct Placement {
  Placement();
  Placement(const Placement&);
  Placement& operator=(const Placement&);
  ~Placement();

  // Empty: the active space.
  std::string space_uuid;
  // Empty: the space's Pinned root.
  std::string parent_uuid;
  // Empty: at the end.
  std::optional<syncer::UniquePosition> position;
};

class RoomModel {
 public:
  RoomModel();
  virtual ~RoomModel();

  // Whether the shared item is here (a member who just joined doesn't have
  // it until the first records apply).
  virtual bool HasRoot() const = 0;
  // The shared item's records as the model has them; |mirror| (the room's as
  // last synced) gives the positions to keep and the fields this version
  // doesn't know. Nullopt while something isn't loaded.
  virtual std::optional<Records> GetLocal(const Records& mirror) const = 0;
  // Applies the room's records: |changed| keys of |mirror| came or changed,
  // |deleted| went (as they were). Creates the shared item if it isn't here.
  virtual void Apply(const Records& mirror,
                     const std::set<std::string>& changed,
                     const Records& deleted) = 0;
  // The easels the shared item holds (a shared easel, or those whose tabs
  // are in a shared folder or space's Pinned), which sync with it.
  virtual std::set<std::string> GetEasels() const = 0;
  // Whether |node| is part of the shared item (read-only for viewers).
  virtual bool Contains(const SidebarNode& node) const = 0;

  // Other rooms' items inside this one (a folder shared on its own in a
  // shared space or folder), by UUID: they're theirs, so this room neither
  // writes nor applies them.
  void set_excluded(std::set<std::string> excluded) {
    excluded_ = std::move(excluded);
  }

 protected:
  std::set<std::string> excluded_;
};

// A shared pinned folder |root_uuid|: the folder (its name, collapse) and
// everything in it, and the easels of the easel tabs in it. A member's
// folder goes where |placement| says when it first comes.
std::unique_ptr<RoomModel> CreateFolderRoomModel(
    SpaceList* list,
    EaselSource* easels,
    std::string root_uuid,
    base::RepeatingCallback<Placement()> placement);

// A shared space |root_uuid|: its name and theme, its Pinned tree and the
// easels of the easel tabs there. Today and the archive stay personal.
std::unique_ptr<RoomModel> CreateSpaceRoomModel(SpaceList* list,
                                                EaselSource* easels,
                                                std::string root_uuid);

// A shared easel |root_uuid|: the board, its objects and its images. When it
// first comes, a pinned tab for it, |tab_uuid| (RoomTabKey()), goes where
// |placement| says (in |list|).
std::unique_ptr<RoomModel> CreateEaselRoomModel(
    SpaceList* list,
    EaselSource* easels,
    std::string root_uuid,
    std::string tab_uuid,
    base::RepeatingCallback<Placement()> placement);

}  // namespace compass::rooms

#endif  // COMPASS_BROWSER_ROOMS_ROOM_MODEL_H_
