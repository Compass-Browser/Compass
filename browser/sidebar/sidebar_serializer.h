// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_SIDEBAR_SIDEBAR_SERIALIZER_H_
#define STRATA_BROWSER_SIDEBAR_SIDEBAR_SERIALIZER_H_

#include <cstddef>

#include "base/values.h"

class GURL;

namespace strata {

class SpaceList;
class SpaceModel;

// The on-disk form of a profile's spaces:
//   {"version": 2, "active": "<space id>", "spaces": [<space>...]}
//   <space> = {"id": "...", "name": "...", "color": "grey"|"blue"|...,
//              "pinned": [<node>...], "today": [<node>...]}
//   <node> = {"type": "tab"|"folder"|"split", "title": "...", "url": "...",
//             "children": [<node>...]}
// Version 1 files held one space's "pinned" and "today" at the top level.
// Today holds tabs and splits; its tabs that aren't worth restoring (see
// IsRestorableURL()) are left out. Node IDs aren't stored; they're only stable
// for a session.
base::DictValue SerializeSpaceList(const SpaceList& list);

// Reads |data| into |list| and returns how many spaces it read. |list|'s
// active space becomes the saved active space: it takes its ID, its name (if
// it has one) and its color, and the saved entries go in ahead of its own. The other saved spaces are added
// around it, in the saved order. Unknown versions are ignored.
size_t DeserializeSpaceList(const base::DictValue& data, SpaceList& list);

// One space, without its version: {"id", "name", "color", "pinned", "today"}.
base::DictValue SerializeSpace(const SpaceModel& model);

// Inserts the entries of |data| (a space as above) into |model|, ahead of
// each section's own entries, and returns how many top-level entries were
// inserted. Entries the model can't hold (an unknown type, a folder in a split
// or in Today) are skipped with their subtrees. The ID, name and color are
// left to the caller.
size_t DeserializeSpace(const base::DictValue& data, SpaceModel& model);

// Whether a Today tab at |url| is worth keeping once its tab is gone. New tab
// and blank pages aren't.
bool IsRestorableURL(const GURL& url);

}  // namespace strata

#endif  // STRATA_BROWSER_SIDEBAR_SIDEBAR_SERIALIZER_H_
