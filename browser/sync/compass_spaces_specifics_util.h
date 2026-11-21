// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_COMPASS_SPACES_SPECIFICS_UTIL_H_
#define COMPASS_BROWSER_SYNC_COMPASS_SPACES_SPECIFICS_UTIL_H_

#include <optional>
#include <string>
#include <vector>

#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"
#include "components/sync/base/unique_position.h"

// How the spaces' model reads and writes CompassSpacesSpecifics, shared by
// COMPASS_SPACES's bridge (compass_spaces_sync_bridge.cc, which defines
// these) and rooms (compass/browser/rooms/room_model.cc), which carry the
// same records for shared spaces, folders and easels (docs/design/sync.md,
// "Rooms: clients").
namespace compass::sync::spaces_util {

// CompassNode.type <-> SidebarNode::Type; nullopt (0) for one this version
// doesn't know (or the root).
std::optional<SidebarNode::Type> NodeTypeFromProto(int type);
int NodeTypeToProto(SidebarNode::Type type);

// The node's own fields (what it shows, not where it is) into |proto|,
// keeping fields that already read the same.
void FillNodeContent(const SidebarNode& node, sync_pb::CompassNode& proto);

// A remote live folder's source, or nullopt if it has none.
std::optional<LiveSource> LiveSourceFromProto(const sync_pb::CompassNode& node);

// |proto| as a position, if valid.
std::optional<syncer::UniquePosition> ValidPosition(
    const sync_pb::UniquePosition& proto);

// A space's theme (color, gradient, intensity, grain, appearance, icon) to
// and from its entity.
void ThemeToProto(const SpaceModel& model, sync_pb::CompassSpace& space);
void ThemeFromProto(const sync_pb::CompassSpace& space, SpaceModel& model);

// The images (CompassEaselAsset sha256) an easel object shows.
std::vector<std::string> EaselObjectAssets(
    const sync_pb::CompassEaselObject& object);

// A remote node's content (URL, titles, saved page, last use, a folder's
// name, collapse, name source, live source) into |node|, of the same type,
// through |model|'s setters. |keep_page|: a tab keeps its URL and title (a
// change of its own waits to be sent).
void ApplyNodeContent(SpaceModel& model,
                      SidebarNode* node,
                      const sync_pb::CompassNode& remote,
                      bool keep_page);

}  // namespace compass::sync::spaces_util

#endif  // COMPASS_BROWSER_SYNC_COMPASS_SPACES_SPECIFICS_UTIL_H_
