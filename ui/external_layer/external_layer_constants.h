// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_UI_EXTERNAL_LAYER_EXTERNAL_LAYER_CONSTANTS_H_
#define COMPASS_UI_EXTERNAL_LAYER_EXTERNAL_LAYER_CONSTANTS_H_

namespace compass {

// The size, in points, of the root layer of an external CoreAnimation
// context. The process drawing it (AVPlayerLayer, videoGravity resize) keeps
// this size; the GPU process scales the CALayerHost to the quad's rect, so the
// two never have to agree on a size that changes.
inline constexpr double kExternalLayerWidth = 1920;
inline constexpr double kExternalLayerHeight = 1080;

}  // namespace compass

#endif  // COMPASS_UI_EXTERNAL_LAYER_EXTERNAL_LAYER_CONSTANTS_H_
