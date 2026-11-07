// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SIDEBAR_SIDEBAR_SERIALIZER_H_
#define COMPASS_BROWSER_SIDEBAR_SIDEBAR_SERIALIZER_H_

#include <cstddef>

#include "base/values.h"

class GURL;

namespace compass {

class SidebarNode;
class SpaceList;
class SpaceModel;

// The on-disk form of a profile's spaces:
//   {"version": 2, "active": "<space id>", "spaces": [<space>...],
//    "favorites": [<node>...]}
//   <space> = {"id": "...", "name": "...", "color": "grey"|"blue"|...,
//              "archive_after_hours": 12, "pinned": [<node>...],
//              "today": [<node>...], "archive": [<archived>...]}
//   <node> = {"id": "<uuid>", "type": "tab"|"folder"|"split", "title": "...",
//             "url": "...", "last_active": "<time>", "children": [<node>...]}
//   <archived> = {"id": "<uuid>", "archived": "<time>",
//                 "tabs": [{"id", "title", "url"}...]}
// Times are base::TimeToValue() strings. "last_active" is only on tabs that
// have one; "archive_after_hours" is 0 for never, and missing in older files
// (the default applies).
// "favorites" holds tabs only, and is missing in older files.
// Version 1 files held one space's "pinned" and "today" at the top level.
// Today holds tabs and splits; its tabs that aren't worth restoring (see
// IsRestorableURL()) are left out. Node IDs are stored (sync identifies nodes
// by them); older files have none, and their nodes get fresh ones, as do
// nodes whose ID is invalid or already taken in their space. Archived entries
// keep their IDs the same way (older files have none).
base::DictValue SerializeSpaceList(const SpaceList& list);

// Reads |data| into |list| and returns how many spaces it read. |list|'s
// active space becomes the saved active space: it takes its ID, its name (if
// it has one) and its color, and the saved entries go in ahead of its own. The other saved spaces are added
// around it, in the saved order. Saved favorites go in ahead of |list|'s.
// Unknown versions are ignored.
size_t DeserializeSpaceList(const base::DictValue& data, SpaceList& list);

// One space, without its version: {"id", "name", "color",
// "archive_after_hours", "pinned", "today", "archive"}.
base::DictValue SerializeSpace(const SpaceModel& model);

// Inserts the entries of |data| (a space as above) into |model|, ahead of
// each section's own entries, and returns how many top-level entries were
// inserted. Entries the model can't hold (an unknown type, a folder in a split
// or in Today) are skipped with their subtrees. Archived entries are appended
// to the model's archive. The ID, name, color and archive delay are left to
// the caller.
size_t DeserializeSpace(const base::DictValue& data, SpaceModel& model);

// One entry (a tab, split or folder) and its subtree, as <node> above, with
// every tab kept, restorable or not. For closed entries that come back
// (closed_entries.h).
base::DictValue SerializeEntry(const SidebarNode& entry);

// Adds the entry |data| (from SerializeEntry()) to |parent| at |index| and
// returns it, or null if |parent| can't hold it. Nodes keep their saved IDs
// unless |model| already has them. A split saved with one pane comes back as
// that pane.
SidebarNode* DeserializeEntry(const base::DictValue& data,
                              SpaceModel& model,
                              SidebarNode* parent,
                              size_t index);

// Whether a Today tab at |url| is worth keeping once its tab is gone. New tab
// and blank pages aren't.
bool IsRestorableURL(const GURL& url);

}  // namespace compass

#endif  // COMPASS_BROWSER_SIDEBAR_SIDEBAR_SERIALIZER_H_
