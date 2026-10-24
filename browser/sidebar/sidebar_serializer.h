// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_SIDEBAR_SIDEBAR_SERIALIZER_H_
#define STRATA_BROWSER_SIDEBAR_SIDEBAR_SERIALIZER_H_

#include <cstddef>

#include "base/values.h"

class GURL;

namespace strata {

class SpaceModel;

// The on-disk form of a space:
//   {"version": 1, "pinned": [<node>...], "today": [<node>...]}
//   <node> = {"type": "tab"|"folder"|"split", "title": "...", "url": "...",
//             "children": [<node>...]}
// Today holds tabs and splits; its tabs that aren't worth restoring (see
// IsRestorableURL()) are left out. Node IDs aren't stored; they're only stable
// for a session.
base::DictValue SerializeSpace(const SpaceModel& model);

// Inserts the entries of |data| into |model|, ahead of each section's own
// entries, and returns how many top-level entries were inserted. Entries the
// model can't hold (an unknown type, a folder in a split or in Today) are
// skipped with their subtrees.
size_t DeserializeSpace(const base::DictValue& data, SpaceModel& model);

// Whether a Today tab at |url| is worth keeping once its tab is gone. New tab
// and blank pages aren't.
bool IsRestorableURL(const GURL& url);

}  // namespace strata

#endif  // STRATA_BROWSER_SIDEBAR_SIDEBAR_SERIALIZER_H_
