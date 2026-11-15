// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_SPACE_IMPORT_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_SPACE_IMPORT_H_

#include <vector>

#include "base/time/time.h"
#include "compass/browser/browser_import/source.h"

namespace compass {
class SpaceList;
class SpaceModel;
}  // namespace compass

namespace compass::browser_import {

struct AddedSidebar {
  AddedSidebar();
  AddedSidebar(AddedSidebar&&);
  AddedSidebar& operator=(AddedSidebar&&);
  ~AddedSidebar();

  // The spaces made, in order.
  std::vector<SpaceModel*> spaces;
  // Tabs added to Pinned, Today and Favorites.
  int pinned = 0;
  int today = 0;
  int favorites = 0;
};

// Adds an imported profile's |sidebar| to the Compass profile's |list|
// (docs/design/browser-import.md, "Mapping"): its spaces after the user's,
// with their Pinned and Today entries (Today's counting as used at |now|),
// and its favorites at the end of the Favorites (those whose page is there
// already left out). A space imported before (its derived ID is in |list|)
// is left as it is. Spaces without a color of their own get one no space
// has yet, so imported spaces tell apart. Entries come in unloaded.
AddedSidebar AddImportedSidebar(SpaceList& list,
                                const ImportedSidebar& sidebar,
                                base::Time now);

// Whether |space| was imported into |list| before.
bool IsImported(const SpaceList& list, const arc::Space& space);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_SPACE_IMPORT_H_
