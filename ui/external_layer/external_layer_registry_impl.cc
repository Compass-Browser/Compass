// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/ui/external_layer/external_layer_registry_impl.h"

#include <memory>

#include "base/containers/flat_set.h"
#include "compass/ui/external_layer/external_layer_registry.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"

namespace compass {
namespace {

class ExternalLayerRegistryImpl : public mojom::ExternalLayerRegistry {
 public:
  ExternalLayerRegistryImpl() = default;
  ExternalLayerRegistryImpl(const ExternalLayerRegistryImpl&) = delete;
  ExternalLayerRegistryImpl& operator=(const ExternalLayerRegistryImpl&) =
      delete;

  ~ExternalLayerRegistryImpl() override {
    for (const auto& id : registered_) {
      ::compass::ExternalLayerRegistry::Get().Unregister(id);
    }
  }

  // mojom::ExternalLayerRegistry:
  void Register(const base::UnguessableToken& overlay_plane_id,
                uint32_t ca_context_id) override {
    registered_.insert(overlay_plane_id);
    ::compass::ExternalLayerRegistry::Get().Register(overlay_plane_id, ca_context_id);
  }

  void Unregister(const base::UnguessableToken& overlay_plane_id) override {
    registered_.erase(overlay_plane_id);
    ::compass::ExternalLayerRegistry::Get().Unregister(overlay_plane_id);
  }

 private:
  base::flat_set<base::UnguessableToken> registered_;
};

}  // namespace

void BindExternalLayerRegistry(
    mojo::PendingReceiver<mojom::ExternalLayerRegistry> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<ExternalLayerRegistryImpl>(),
                              std::move(receiver));
}

}  // namespace compass
