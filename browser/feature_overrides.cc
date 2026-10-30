// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/feature_overrides.h"

#include "components/variations/feature_overrides.h"
#include "third_party/blink/public/common/features.h"

namespace compass {

void RegisterFeatureOverrides(variations::FeatureOverrides& overrides) {
  // A playing, audible video goes into picture-in-picture when its tab is
  // switched away from, whether or not the site asked for it
  // (docs/design/picture-in-picture.md).
  overrides.EnableFeature(
      blink::features::kBrowserInitiatedAutomaticPictureInPicture);
}

}  // namespace compass
