// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_SIDEBAR_SIDEBAR_SERIALIZER_H_
#define STRATA_BROWSER_SIDEBAR_SIDEBAR_SERIALIZER_H_

#include <cstddef>

#include "base/values.h"

namespace strata {

class SpaceModel;

// The on-disk form of a space's pinned section:
//   {"version": 1, "pinned": [<node>...]}
//   <node> = {"type": "tab"|"folder"|"split", "title": "...", "url": "...",
//             "children": [<node>...]}
// Node IDs aren't stored; they're only stable for a session.
base::DictValue SerializePinned(const SpaceModel& model);

// Inserts the entries of |data| into |model|'s pinned section, starting at
// |index|, and returns how many top-level entries were inserted. Entries the
// model can't hold (an unknown type, a folder in a split) are skipped with
// their subtrees.
size_t DeserializePinned(const base::DictValue& data,
                         SpaceModel& model,
                         size_t index);

}  // namespace strata

#endif  // STRATA_BROWSER_SIDEBAR_SIDEBAR_SERIALIZER_H_
