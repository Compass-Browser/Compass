// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ARC_IMPORT_SIDEBAR_IMPORT_H_
#define COMPASS_BROWSER_ARC_IMPORT_SIDEBAR_IMPORT_H_

#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "base/uuid.h"
#include "compass/browser/arc_import/arc_sidebar.h"

namespace compass {
class SpaceList;
class SpaceModel;
}  // namespace compass

namespace compass::arc {

// Adds an imported Arc profile's sidebar to its Compass profile's |list|
// (docs/design/arc-import.md, "Mapping to Compass"): each of |spaces| (the
// profile's, in Arc's order) as a space with its name and color, its Pinned
// and Today entries (folders, splits, tabs with their own titles), and
// |profile|'s favorites. A new profile's lone empty space takes the first
// one's place. Today's entries count as used at |now|, so Today's
// auto-archive doesn't sweep them up at once. Entries come in unloaded.
// Returns the spaces it filled, in order.
//
// Entries and spaces get IDs derived from Arc's (ArcDerivedId()), so the same
// Arc data imported on two Macs gives the same entries, which merge when the
// Macs sync instead of showing twice (docs/design/sync.md, "Slice 7 notes").
// An ID already in use here (a second import) gets a fresh one instead.
std::vector<SpaceModel*> AddArcSidebar(SpaceList& list,
                                       const Profile& profile,
                                       const std::vector<Space>& spaces,
                                       base::Time now);

// The ID an Arc item or space with |arc_id| gets: a name-based UUID
// (version 8, RFC 9562) from SHA-256 of Arc's ID. Invalid (empty) for an
// empty |arc_id|.
base::Uuid ArcDerivedId(std::string_view arc_id);

}  // namespace compass::arc

#endif  // COMPASS_BROWSER_ARC_IMPORT_SIDEBAR_IMPORT_H_
