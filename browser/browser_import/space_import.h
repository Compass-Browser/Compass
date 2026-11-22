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
//
// With |into_new_profile| (a profile just copied from the browser's, "As a
// profile"), the profile's lone empty space takes the first imported
// space's place instead of staying first.
AddedSidebar AddImportedSidebar(SpaceList& list,
                                const ImportedSidebar& sidebar,
                                base::Time now,
                                bool into_new_profile = false);

// |sidebar| with every space and item ID namespaced for a profile of its
// own, so its spaces derive other Compass IDs than the same browser
// profile's spaces imported into another Compass profile (space IDs are
// unique across profiles: SpaceDirectory).
ImportedSidebar ForNewProfile(ImportedSidebar sidebar);

// Whether |space| was imported into |list| before.
bool IsImported(const SpaceList& list, const arc::Space& space);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_SPACE_IMPORT_H_
