// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_ARC_IMPORT_ARC_EASEL_IMPORT_H_
#define COMPASS_BROWSER_UI_ARC_IMPORT_ARC_EASEL_IMPORT_H_

#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "compass/browser/arc_import/arc_easels.h"

class Profile;

namespace compass::arc {

// Arc's boards, read once for an import and shared by its profiles.
using SharedArcEasels = base::RefCountedData<ArcEasels>;

struct ArcEaselImportCounts {
  // Boards brought over with their objects.
  int imported = 0;
  // Boards Arc never downloaded to this Mac: they come over empty (or stay
  // so) until they're opened in Arc and imported again.
  int missing = 0;
};

// Writes the boards |wanted| (those the profile's imported Arc spaces point
// at) into |profile|'s EaselStore once it has loaded, with IDs derived from
// Arc's (ArcEaselUuid(), ArcEaselObjectUuid()):
// - a board Compass doesn't have is made, with Arc's title and background;
// - a board that is there and empty (it was missing before) gets Arc's
//   objects; one with objects is left as it is (the user may have changed
//   it, and syncs it), so importing again never doubles anything.
// Images (Firebase Storage URLs only) are downloaded without credentials,
// fitted by PrepareImage() and kept with AddAsset(); an object whose image
// couldn't be had stays as a link (WithoutImages()). On the UI thread.
void ImportArcEasels(::Profile* profile,
                     std::vector<ArcEaselRef> wanted,
                     scoped_refptr<SharedArcEasels> easels,
                     base::OnceCallback<void(ArcEaselImportCounts)> callback);

}  // namespace compass::arc

#endif  // COMPASS_BROWSER_UI_ARC_IMPORT_ARC_EASEL_IMPORT_H_
