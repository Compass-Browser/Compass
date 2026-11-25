// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_UI_EXTERNAL_LAYER_EXTERNAL_LAYER_REGISTRY_IMPL_H_
#define COMPASS_UI_EXTERNAL_LAYER_EXTERNAL_LAYER_REGISTRY_IMPL_H_

#include "compass/media/avf/public/mojom/avf_player.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"

namespace compass {

// Binds the browser's compass.mojom.ExternalLayerRegistry in the GPU process.
// Contexts registered over a connection are dropped when it closes.
void BindExternalLayerRegistry(
    mojo::PendingReceiver<mojom::ExternalLayerRegistry> receiver);

}  // namespace compass

#endif  // COMPASS_UI_EXTERNAL_LAYER_EXTERNAL_LAYER_REGISTRY_IMPL_H_
