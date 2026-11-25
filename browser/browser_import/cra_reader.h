// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_CRA_READER_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_CRA_READER_H_

#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"
#include "compass/browser/browser_import/source.h"

// Reading Cra's sidebar (docs/design/browser-import.md, "Cra"). Cra is
// Chromium-based: its User Data is ~/Library/Application Support/Cra/
// Chromium, read as any Chromium-based browser's, but its sidebar is its
// own, sidebar.json in the folder above, for every profile at once.
namespace compass::browser_import {

// sidebar.json, beside Cra's |user_data|.
base::FilePath CraSidebarPath(const base::FilePath& user_data);

// The sidebar |document| (sidebar.json) gives the Cra profile |profile|:
// the spaces whose profile it is, in Cra's order (Blank Window spaces left
// out). A space's Pinned and Today keep their folders and splits; its
// favorites come first in its Pinned, or, when Cra shares them across a
// profile's spaces, they become Compass's Favorites. |bookmarks| (Chromium's
// Bookmarks) come as one more space (AddBookmarksSpace()).
ImportedSidebar CraSidebar(const SourceProfile& profile,
                           const base::DictValue& document,
                           std::vector<arc::Item> bookmarks,
                           bool import_tabs);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_CRA_READER_H_
