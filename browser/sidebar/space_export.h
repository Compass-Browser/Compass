// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SIDEBAR_SPACE_EXPORT_H_
#define COMPASS_BROWSER_SIDEBAR_SPACE_EXPORT_H_

#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"

namespace compass {

class SpaceModel;

// Export Spaces (docs/design/spaces.md, "Export"): the spaces as a
// bookmarks file in the Netscape format every browser imports. Each space
// is a folder holding its pinned entries, folders kept, and a "Today"
// folder of its open tabs; the favorites, if any, come first as
// "Favorites". A pinned tab is its own page (saved_url), not wherever it has
// since navigated; a split is its panes.
struct SpaceExport {
  SpaceExport();
  SpaceExport(const SpaceExport&);
  SpaceExport& operator=(const SpaceExport&);
  ~SpaceExport();

  // Each profile's favorites, at most one per profile; may be empty.
  std::vector<raw_ptr<const SpaceModel>> favorites;
  std::vector<raw_ptr<const SpaceModel>> spaces;
};

std::string SpacesToBookmarksHtml(const SpaceExport& spaces);

}  // namespace compass

#endif  // COMPASS_BROWSER_SIDEBAR_SPACE_EXPORT_H_
