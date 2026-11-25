// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/ui/external_layer/external_layer_registry.h"

namespace compass {

// static
ExternalLayerRegistry& ExternalLayerRegistry::Get() {
  static base::NoDestructor<ExternalLayerRegistry> instance;
  return *instance;
}

ExternalLayerRegistry::ExternalLayerRegistry() = default;
ExternalLayerRegistry::~ExternalLayerRegistry() = default;

void ExternalLayerRegistry::Register(
    const base::UnguessableToken& overlay_plane_id,
    uint32_t ca_context_id) {
  base::AutoLock lock(lock_);
  contexts_[overlay_plane_id] = ca_context_id;
}

void ExternalLayerRegistry::Unregister(
    const base::UnguessableToken& overlay_plane_id) {
  base::AutoLock lock(lock_);
  contexts_.erase(overlay_plane_id);
}

std::optional<uint32_t> ExternalLayerRegistry::Lookup(
    const base::UnguessableToken& overlay_plane_id) const {
  base::AutoLock lock(lock_);
  auto it = contexts_.find(overlay_plane_id);
  if (it == contexts_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace compass
