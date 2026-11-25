// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_YAB_READER_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_YAB_READER_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"
#include "compass/browser/browser_import/app_sidebar.h"
#include "compass/browser/browser_import/source.h"

// Reading Yab (macOS; docs/design/browser-import.md, "Yab"). Yab is a WebKit
// browser that keeps everything in JSON files of its own, in
// ~/Library/Application Support/Yab (Fast before October 2026): its spaces
// (spaces.json), each space's tabs (session.json for the first,
// session-<space ID>.json for the others), the pins every space shows
// (session-…0A11.json), bookmarks.json and history.json (history-<ID>.json
// for a space with sign-ins of its own). One profile, the folder.
namespace compass::browser_import {

// The session file of the space |space_id|: the first space's is
// session.json.
base::FilePath YabSessionPath(const base::FilePath& root,
                              std::string_view space_id);
// The session file of the pins every space shows ("Pins in every space").
base::FilePath YabSharedPinsPath(const base::FilePath& root);

// The sidebar Yab's files give, as parsed:
// - |spaces|: spaces.json (null: Yab's one space, "Personal").
// - |sessions|: each space's session file, by space ID.
// - |shared_pins|: the shared pins' session file, when Yab shows them in
//   every space (null otherwise).
// - |bookmarks|: the Pinned entries of bookmarks.json (ParseYabBookmarks()).
// Only the spaces of |profile|'s Yab profile (SourceKey(); every space for
// an empty one) come; the shared pins and the bookmarks only with the
// first profile's.
// A space's pins come first in its Pinned, then its kept tabs (folders
// nested); its other tabs go to Today; side-by-side tabs as splits. The
// shared pins become Compass's Favorites; bookmarks one more space,
// "Yab" (AddBookmarksSpace()).
ImportedSidebar YabSidebar(
    const SourceProfile& profile,
    const base::ListValue* spaces,
    const std::map<std::string, const base::DictValue*>& sessions,
    const base::DictValue* shared_pins,
    std::vector<arc::Item> bookmarks);

// bookmarks.json's tree as Pinned entries, folders nested. IDs:
// |id_prefix| + "bookmark:" + Yab's ID.
std::vector<arc::Item> ParseYabBookmarks(const base::ListValue& bookmarks,
                                         std::string_view id_prefix);

// A history file's pages (history.json: [{url, title, count, last, typed}],
// times in seconds since 2001).
std::vector<HistoryEntry> ParseYabHistory(const base::ListValue& history);

// Yab's profiles, in the order of their first space: a space with
// sign-ins of its own has its own (or shares another's, signInWith), the
// others the first space's. spaces.json as parsed (null: Yab's one space).
std::vector<AppProfile> ParseYabProfiles(const base::ListValue* spaces);

// The Chromium profile folder of the Yab profile |key| under Yab's folder
// |app| (where its Chromium engine keeps cookies for the sites it opened):
// the one ChromiumProfiles/<key>-shared|named.json records, else
// Chromium/space-<key>. It may not exist.
base::FilePath YabChromiumFolder(const base::FilePath& app,
                                 std::string_view key);

// Reads |profile|'s sidebar (with bookmarks and tabs as asked). Its root is
// Yab's Chromium folder, inside Yab's own. Blocking.
ImportedSidebar ReadYabSidebar(const SourceProfile& profile,
                               bool import_bookmarks,
                               bool import_tabs);
// The history of the Yab profile |key| in Yab's folder |root|
// (history.json for the first, history-<key>.json for the others; every
// one for an empty |key|). Blocking.
std::vector<HistoryEntry> ReadYabHistory(const base::FilePath& root,
                                         std::string_view key,
                                         size_t max_entries);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_YAB_READER_H_
