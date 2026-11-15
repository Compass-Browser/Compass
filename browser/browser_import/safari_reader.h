// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_SAFARI_READER_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_SAFARI_READER_H_

#include <optional>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"
#include "build/build_config.h"
#include "compass/browser/browser_import/chromium_reader.h"
#include "compass/browser/browser_import/source.h"

// Reading Safari's data (macOS): Bookmarks.plist, the last session's
// windows (LastSession.plist) and History.db. The property lists are read
// as base::Values (ReadPlist()), so the parsing is the same everywhere and
// tested on every platform. See docs/design/browser-import.md, "Safari".
namespace compass::browser_import {

// The Pinned entries Bookmarks.plist gives: the Favorites bar's
// (BookmarksBar) entries, then "Bookmarks Menu", other top-level folders
// and "Reading List" as folders (when not empty). IDs: |id_prefix| + the
// bookmark's UUID.
std::vector<arc::Item> ParseSafariBookmarks(const base::DictValue& plist,
                                            std::string_view id_prefix);

// The tabs of LastSession.plist's windows, private windows left out.
std::vector<SessionTab> ParseSafariLastSession(const base::DictValue& plist);

// History.db's pages, the most recently visited first, at most |max|. Reads
// a copy. Blocking.
std::vector<HistoryEntry> ReadSafariHistory(const base::FilePath& history,
                                            size_t max);

// Safari's sidebar: one space, "Safari", with the bookmarks in Pinned and
// the last session's tabs in Today.
ImportedSidebar SafariSidebar(const SourceProfile& profile,
                              std::vector<arc::Item> bookmarks,
                              const std::vector<SessionTab>& tabs);

// Where Safari's last session may be: its container first, then
// ~/Library/Safari (|safari_dir|).
std::vector<base::FilePath> SafariLastSessionPaths(
    const base::FilePath& safari_dir);

#if BUILDFLAG(IS_MAC)
// A property list file (XML or binary) as a dictionary; nullopt if it
// can't be read (missing, or Compass lacks Full Disk Access) or isn't one.
// Dates become seconds since 2001, data is left out. Blocking.
std::optional<base::DictValue> ReadPlist(const base::FilePath& path);
#endif

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_SAFARI_READER_H_
