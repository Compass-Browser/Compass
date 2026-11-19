// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_EASELS_EASEL_SYNC_H_
#define COMPASS_BROWSER_EASELS_EASEL_SYNC_H_

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"

namespace compass::easels {

class EaselStore;

// What the spaces' sync delegates (desktop compass_spaces_sync.cc, Android
// compass_spaces_sync_android.cc) hand the bridge for Easels
// (CompassSpacesSyncBridge::Delegate::GetLocalEasels() and the rest).

// Every board, object and image (without bytes); nullopt until loaded, or
// without a store.
std::optional<std::vector<sync_pb::CompassSpacesSpecifics>> LocalEaselEntities(
    const EaselStore* store);
// An image's bytes.
std::optional<std::string> EaselAssetData(const EaselStore* store,
                                          const std::string& sha256);
// Remote changes into the store.
void ApplyRemoteEasels(
    EaselStore* store,
    const std::vector<sync_pb::CompassSpacesSpecifics>& changed,
    const std::set<std::string>& deleted);

}  // namespace compass::easels

#endif  // COMPASS_BROWSER_EASELS_EASEL_SYNC_H_
