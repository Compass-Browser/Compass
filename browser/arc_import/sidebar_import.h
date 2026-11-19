// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ARC_IMPORT_SIDEBAR_IMPORT_H_
#define COMPASS_BROWSER_ARC_IMPORT_SIDEBAR_IMPORT_H_

#include <optional>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "base/uuid.h"
#include "compass/browser/arc_import/arc_sidebar.h"

namespace compass {
class SidebarNode;
class SpaceList;
class SpaceModel;
}  // namespace compass

namespace compass::arc {

// Where AddArcSidebar() adds to.
enum class ArcImportInto {
  // A profile just copied from Arc: its lone empty space takes the first
  // Arc space's place, and Arc's favorites come along.
  kNewProfile,
  // A profile imported before, getting Arc spaces added since: they go
  // after its spaces, its favorites stay as they are.
  kImportedProfile,
};

// Adds an imported Arc profile's sidebar to its Compass profile's |list|
// (docs/design/arc-import.md, "Mapping to Compass"): each of |spaces| (the
// profile's, in Arc's order) as a space with its name and color, its Pinned
// and Today entries (folders, splits, tabs with their own titles), and, for
// a new profile, |profile|'s favorites. Today's entries count as used at
// |now|, so Today's auto-archive doesn't sweep them up at once. Entries come
// in unloaded. Returns the spaces it filled, in order.
//
// Entries and spaces get IDs derived from Arc's (ArcDerivedId()), so the same
// Arc data imported on two Macs gives the same entries, which merge when the
// Macs sync instead of showing twice (docs/design/sync.md, "First sync on a
// device that already has spaces"). An ID already in use here (a second
// import) gets a fresh one instead.
std::vector<SpaceModel*> AddArcSidebar(SpaceList& list,
                                       const Profile& profile,
                                       const std::vector<Space>& spaces,
                                       base::Time now,
                                       ArcImportInto into =
                                           ArcImportInto::kNewProfile);

// Adds |items| at the end of |parent|, a node of |model| (one of |list|'s
// spaces or its favorites), as AddArcSidebar() adds a space's: folders,
// splits and tabs with derived IDs (ArcDerivedId() of Item::id) where free.
// |last_active| overrides the tabs' own last active times. An easel becomes
// a tab of its board (ArcEaselUuid()); the board itself is written by the
// importer (arc_easels.h). For other browsers' imports too
// (docs/design/browser-import.md).
void AddItems(SpaceList& list,
              SpaceModel& model,
              SidebarNode* parent,
              const std::vector<Item>& items,
              std::optional<base::Time> last_active);

// The ID an Arc item or space with |arc_id| gets: a name-based UUID
// (version 8, RFC 9562) from SHA-256 of Arc's ID. Invalid (empty) for an
// empty |arc_id|.
base::Uuid ArcDerivedId(std::string_view arc_id);

// The Compass board (CompassEasel.uuid) an Arc easel with |easel_id| becomes:
// ArcDerivedId() of its Firestore path ("easels/<id>"), so importing again
// or on another Mac finds the same board. Its objects' are derived from
// theirs ("easels/<id>/objects/<object id>").
base::Uuid ArcEaselUuid(std::string_view easel_id);
base::Uuid ArcEaselObjectUuid(std::string_view easel_id,
                              std::string_view object_id);

}  // namespace compass::arc

#endif  // COMPASS_BROWSER_ARC_IMPORT_SIDEBAR_IMPORT_H_
