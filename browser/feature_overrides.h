// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_FEATURE_OVERRIDES_H_
#define STRATA_BROWSER_FEATURE_OVERRIDES_H_

namespace variations {
class FeatureOverrides;
}  // namespace variations

namespace strata {

// Strata's defaults for upstream features, registered at startup by
// ChromeBrowserFieldTrials::RegisterFeatureOverrides() (see patches/).
// --enable-features and --disable-features still win.
void RegisterFeatureOverrides(variations::FeatureOverrides& overrides);

}  // namespace strata

#endif  // STRATA_BROWSER_FEATURE_OVERRIDES_H_
