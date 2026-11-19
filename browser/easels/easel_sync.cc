// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/easels/easel_sync.h"

#include <utility>

#include "compass/browser/easels/easel_store.h"

namespace compass::easels {

std::optional<std::vector<sync_pb::CompassSpacesSpecifics>> LocalEaselEntities(
    const EaselStore* store) {
  if (!store) {
    return std::nullopt;
  }
  std::optional<EaselStore::Entities> entities = store->GetEntities();
  if (!entities) {
    return std::nullopt;
  }
  std::vector<sync_pb::CompassSpacesSpecifics> list;
  list.reserve(entities->easels.size() + entities->objects.size() +
               entities->assets.size());
  for (sync_pb::CompassEasel& easel : entities->easels) {
    *list.emplace_back().mutable_easel() = std::move(easel);
  }
  for (sync_pb::CompassEaselObject& object : entities->objects) {
    *list.emplace_back().mutable_easel_object() = std::move(object);
  }
  for (sync_pb::CompassEaselAsset& asset : entities->assets) {
    *list.emplace_back().mutable_easel_asset() = std::move(asset);
  }
  return list;
}

std::optional<std::string> EaselAssetData(const EaselStore* store,
                                          const std::string& sha256) {
  const sync_pb::CompassEaselAsset* asset =
      store ? store->FindAsset(sha256) : nullptr;
  if (!asset) {
    return std::nullopt;
  }
  return asset->data();
}

void ApplyRemoteEasels(
    EaselStore* store,
    const std::vector<sync_pb::CompassSpacesSpecifics>& changed,
    const std::set<std::string>& deleted) {
  if (!store) {
    return;
  }
  EaselStore::Entities entities;
  for (const sync_pb::CompassSpacesSpecifics& specifics : changed) {
    if (specifics.has_easel()) {
      entities.easels.push_back(specifics.easel());
    } else if (specifics.has_easel_object()) {
      entities.objects.push_back(specifics.easel_object());
    } else if (specifics.has_easel_asset()) {
      entities.assets.push_back(specifics.easel_asset());
    }
  }
  store->ApplyRemote(std::move(entities), deleted);
}

}  // namespace compass::easels
