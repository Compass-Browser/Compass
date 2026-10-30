// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_FEATURE_OVERRIDES_H_
#define COMPASS_BROWSER_FEATURE_OVERRIDES_H_

namespace variations {
class FeatureOverrides;
}  // namespace variations

namespace compass {

// Compass's defaults for upstream features, registered at startup by
// ChromeBrowserFieldTrials::RegisterFeatureOverrides() (see patches/).
// --enable-features and --disable-features still win.
void RegisterFeatureOverrides(variations::FeatureOverrides& overrides);

}  // namespace compass

#endif  // COMPASS_BROWSER_FEATURE_OVERRIDES_H_
