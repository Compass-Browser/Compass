// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ARC_IMPORT_ARC_EASELS_H_
#define COMPASS_BROWSER_ARC_IMPORT_ARC_EASELS_H_

#include <map>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "compass/browser/arc_import/arc_sidebar.h"
#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"
#include "url/gurl.h"

// Reading Arc's Easels (docs/design/easels.md, "Arc's Easels"): the boards
// in Arc's Firestore cache, as Compass's objects. No browser dependencies.
namespace compass::arc {

// How wide an Arc board is in Compass, in board points. Arc keeps an
// object's box in fractions of the board's width (x, y, width and height
// alike), so this sets the scale.
inline constexpr double kArcEaselWidth = 1000;

// An object of an Arc board.
struct ArcEaselObject {
  ArcEaselObject();
  ArcEaselObject(const ArcEaselObject&);
  ArcEaselObject(ArcEaselObject&&);
  ArcEaselObject& operator=(const ArcEaselObject&);
  ArcEaselObject& operator=(ArcEaselObject&&);
  ~ArcEaselObject();

  // Arc's object ID (its Firestore document's).
  std::string id;
  // Box, z and content, mapped to Compass's. No uuid or board yet, and no
  // image assets: the importer downloads |image_url| and |favicon_url| and
  // fills them in (image.asset, link.image_asset, capture.image_asset and
  // favicon_asset).
  sync_pb::CompassEaselObject object;
  // An image's picture, a link's preview or a capture's image; empty if
  // none.
  GURL image_url;
  // A capture's page icon; empty if none.
  GURL favicon_url;
};

// One of Arc's boards.
struct ArcEasel {
  ArcEasel();
  ArcEasel(const ArcEasel&);
  ArcEasel(ArcEasel&&);
  ArcEasel& operator=(const ArcEasel&);
  ArcEasel& operator=(ArcEasel&&);
  ~ArcEasel();

  // Arc's ID (its Firestore document's, and the sidebar's easelID).
  std::string id;
  // Empty if the board's own document isn't in the cache.
  std::string title;
  // Compass's background for Arc's palette (CompassEasel.palette).
  std::string palette;
  base::Time last_modified;
  // Whether Arc downloaded the board's objects to this Mac: it does when the
  // board is opened. A board never opened here has none, not even its title.
  bool has_objects = false;
  // Lowest z first. Arc's title object is left out: Compass shows the
  // board's title itself.
  std::vector<ArcEaselObject> objects;
};

// By Arc's ID.
using ArcEasels = std::map<std::string, ArcEasel>;

// A board Arc's sidebar points at.
struct ArcEaselRef {
  std::string id;
  // The sidebar's title for it.
  std::u16string title;
};

// The boards |spaces| point at (Pinned, Today, in folders and splits), each
// once, in order.
std::vector<ArcEaselRef> EaselsIn(const std::vector<Space>& spaces);

// Reads the boards in Arc's Firestore cache |dir| (ArcPaths::easels), a
// LevelDB of Firestore's client documents. Copies it to a temporary folder
// first (Arc holds a lock on it while it runs), opens the copy, and reads
// every board and object it has; a document cached more than once counts
// in its latest version, a deleted one not at all. Empty if there's no
// cache or it can't be read. Blocking.
ArcEasels ReadArcEasels(const base::FilePath& dir);

// Whether the importer may download |url|: Arc's images are Firebase
// Storage download URLs (their token is in the URL), fetched over HTTPS
// without signing in.
bool IsArcEaselImageUrl(const GURL& url);

// |object| for when its images couldn't be had: an image becomes a link to
// it, a capture a link to its page, a link loses its preview. Other
// objects are as they are.
sync_pb::CompassEaselObject WithoutImages(const ArcEaselObject& object);

}  // namespace compass::arc

#endif  // COMPASS_BROWSER_ARC_IMPORT_ARC_EASELS_H_
