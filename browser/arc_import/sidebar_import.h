// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ARC_IMPORT_SIDEBAR_IMPORT_H_
#define COMPASS_BROWSER_ARC_IMPORT_SIDEBAR_IMPORT_H_

#include <vector>

#include "base/time/time.h"
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
std::vector<SpaceModel*> AddArcSidebar(SpaceList& list,
                                       const Profile& profile,
                                       const std::vector<Space>& spaces,
                                       base::Time now);

}  // namespace compass::arc

#endif  // COMPASS_BROWSER_ARC_IMPORT_SIDEBAR_IMPORT_H_
