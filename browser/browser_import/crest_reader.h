// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_CREST_READER_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_CREST_READER_H_

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"
#include "compass/browser/browser_import/app_sidebar.h"
#include "compass/browser/browser_import/source.h"

// Reading Crest (macOS; docs/design/browser-import.md, "Crest"). Crest opens
// pages in Chromium, a Chromium profile per space (Crest-<profile ID> in
// ~/Library/Application Support/Crest/Chromium), but keeps its sidebar and
// history itself: in ControlPlane/session.sqlite, a table of JSON rows
// ("core": the spaces and their tabs; "history.<space ID>": a space's
// history). One profile, the folder. Its passwords are in the Keychain under
// Crest's own access group, which no other app can read.
namespace compass::browser_import {

// ControlPlane/session.sqlite under Crest's |root|.
base::FilePath CrestSessionPath(const base::FilePath& root);

// What session.sqlite holds that Compass imports.
struct CrestSession {
  CrestSession();
  CrestSession(CrestSession&&);
  CrestSession& operator=(CrestSession&&);
  ~CrestSession();

  // The "core" row.
  std::optional<base::DictValue> core;
  // The "history.<ID>" rows, by space ID.
  std::map<std::string, base::ListValue> history;
};

// Reads |path| (a copy of it: Crest may have it open). nullopt if it can't
// be read. Blocking.
std::optional<CrestSession> ReadCrestSession(const base::FilePath& path,
                                             bool with_history);

// Crest's profiles, in the order of their first space: each space browses
// with its own (profile.id), or a Sub-Space with its group's
// (sharedProfileID). Its Chromium profile is Chromium/Crest-<UUID>.
std::vector<AppProfile> ParseCrestProfiles(const base::DictValue& core);
// The IDs of the spaces of the profile |key|.
std::vector<std::string> CrestSpaceIds(const base::DictValue& core,
                                       std::string_view key);

// The sidebar |core| gives the Crest profile |profile| (SourceKey(): its
// UUID): its spaces, in Crest's order, with their names, symbols and colors. A
// space's pinned icons come first in its Pinned, then its saved tabs, folders
// nested; its open tabs go to Today, folders as folders; split groups as
// splits.
ImportedSidebar CrestSidebar(const SourceProfile& profile,
                             const base::DictValue& core);

// The pages of a space's history row ([{url, title, lastVisitedAt,
// visitCount}], times in seconds since 2001).
std::vector<HistoryEntry> ParseCrestHistory(const base::ListValue& history);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_CREST_READER_H_
