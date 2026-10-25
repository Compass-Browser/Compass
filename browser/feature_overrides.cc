// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/feature_overrides.h"

#include "components/variations/feature_overrides.h"
#include "third_party/blink/public/common/features.h"

namespace strata {

void RegisterFeatureOverrides(variations::FeatureOverrides& overrides) {
  // A playing, audible video goes into picture-in-picture when its tab is
  // switched away from, whether or not the site asked for it
  // (docs/design/picture-in-picture.md).
  overrides.EnableFeature(
      blink::features::kBrowserInitiatedAutomaticPictureInPicture);
}

}  // namespace strata
