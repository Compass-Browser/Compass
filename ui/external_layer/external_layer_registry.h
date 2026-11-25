// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_UI_EXTERNAL_LAYER_EXTERNAL_LAYER_REGISTRY_H_
#define COMPASS_UI_EXTERNAL_LAYER_EXTERNAL_LAYER_REGISTRY_H_

#include <stdint.h>

#include <optional>

#include "base/component_export.h"
#include "base/containers/flat_map.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/unguessable_token.h"

namespace compass {

// In the GPU process: which CoreAnimation context (a CAContext in another
// process, e.g. AVPlayer's layer in the AVFoundation utility process) the CA
// renderer hosts where a video-hole quad with a given overlay plane id is
// drawn. Only the browser registers contexts (compass.mojom.
// ExternalLayerRegistry); renderers only know the unguessable plane id.
class COMPONENT_EXPORT(COMPASS_EXTERNAL_LAYER) ExternalLayerRegistry {
 public:
  static ExternalLayerRegistry& Get();

  ExternalLayerRegistry(const ExternalLayerRegistry&) = delete;
  ExternalLayerRegistry& operator=(const ExternalLayerRegistry&) = delete;

  void Register(const base::UnguessableToken& overlay_plane_id,
                uint32_t ca_context_id);
  void Unregister(const base::UnguessableToken& overlay_plane_id);
  std::optional<uint32_t> Lookup(
      const base::UnguessableToken& overlay_plane_id) const;

 private:
  friend class base::NoDestructor<ExternalLayerRegistry>;
  ExternalLayerRegistry();
  ~ExternalLayerRegistry();

  mutable base::Lock lock_;
  base::flat_map<base::UnguessableToken, uint32_t> contexts_
      GUARDED_BY(lock_);
};

}  // namespace compass

#endif  // COMPASS_UI_EXTERNAL_LAYER_EXTERNAL_LAYER_REGISTRY_H_
