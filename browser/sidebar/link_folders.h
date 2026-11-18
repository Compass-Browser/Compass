// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SIDEBAR_LINK_FOLDERS_H_
#define COMPASS_BROWSER_SIDEBAR_LINK_FOLDERS_H_

#include <string>

#include "base/containers/flat_map.h"
#include "base/uuid.h"
#include "url/gurl.h"

namespace compass {

class SidebarNode;
class SpaceModel;

// A link folder's name: its opener's (a split's first pane's) name or title,
// else its host.
std::u16string GetLinkFolderTitle(const SidebarNode& opener);

// Link folders (docs/design/sidebar.md, "Link folders"), for clients that
// add a link's entry themselves rather than through the desktop's strip
// (Android; the iOS app's LinkFolders is the same in Swift). A link opened in
// the background from a page in the sidebar gathers with that page in a Today
// folder:
//
//  - The opener loose in Today: a folder named after it takes its place,
//    holding the opener and then the link. A split opener goes in whole.
//  - The opener in a Today folder: the link joins that folder.
//  - The opener pinned, a favorite, or in another space: it stays put; its
//    links gather in a folder at the top of Today, named after it, which
//    later links reuse while it's there.
//  - Within the folder, a link goes after the one the opener opened before
//    (if it's still there), else right after the opener (at the end, for a
//    folder at the top of Today).
//
// Folders made here are marked as this device's link folders
// (SpaceModel::MarkLinkFolder()), which Claude may name.
//
// One per window, as the desktop's TabStripBridge::link_folders_; not saved.
class LinkFolders {
 public:
  LinkFolders();
  LinkFolders(const LinkFolders&) = delete;
  LinkFolders& operator=(const LinkFolders&) = delete;
  ~LinkFolders();

  // Adds a tab entry for |url| to |space|'s Today, in |opener|'s link folder
  // (made if needed). |opener| is the tab node the link came from: one of
  // |space|'s, a split's pane, a favorite or another space's entry. Returns
  // the new node, or null if it couldn't go in a folder (the caller then
  // adds it as usual).
  SidebarNode* AddLink(SpaceModel& space,
                       const SidebarNode& opener,
                       const GURL& url,
                       const std::u16string& title);

 private:
  // The folders at the top of Today for pinned entries, favorites and other
  // spaces' entries, by the opener's ID.
  base::flat_map<base::Uuid, base::Uuid> folders_;
  // The link each opener opened last, by the opener's ID.
  base::flat_map<base::Uuid, base::Uuid> last_link_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_SIDEBAR_LINK_FOLDERS_H_
