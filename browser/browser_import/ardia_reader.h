// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_ARDIA_READER_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_ARDIA_READER_H_

#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"
#include "compass/browser/browser_import/source.h"

// Reading Ardia's sidebar (docs/design/browser-import.md, "Ardia"). Ardia
// is Chromium-based: its profiles are Chromium profiles in its User Data,
// read as any Chromium-based browser's. Its sidebar is its own: Ardia keeps
// pinned tabs, folders and open tabs in session.json, beside User Data
// (Linux: in the profile, Ardia/linux-session.json), not in Chromium's
// session files. Like Dia, it has no spaces apart from its profiles.
namespace compass::browser_import {

// Where Ardia's session.json may be for |profile|, the first first.
std::vector<base::FilePath> ArdiaSessionPaths(const SourceProfile& profile);

// The sidebar |session| (session.json) gives the Chromium profile
// |profile|: a space for each Ardia profile in that folder (several may
// share one), in Ardia's order, with its name and icon. Its Top Apps come
// first in its Pinned (a Top Apps folder as a folder), then its pinned rows
// and pinned folders; its other tabs, of every window, go to Today, tab
// groups as folders, splits as splits. |bookmarks| (Chromium's Bookmarks)
// come as one more space (AddBookmarksSpace()).
ImportedSidebar ArdiaSidebar(const SourceProfile& profile,
                             const base::DictValue& session,
                             std::vector<arc::Item> bookmarks,
                             bool import_tabs);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_ARDIA_READER_H_
