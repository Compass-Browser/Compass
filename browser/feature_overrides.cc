// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/feature_overrides.h"

#include "build/build_config.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/common/chrome_features.h"
#include "components/autofill/core/common/autofill_debug_features.h"
#include "components/network_time/network_time_tracker.h"
#include "components/omnibox/browser/aim_eligibility_service_features.h"
#include "components/signin/public/base/signin_switches.h"
#include "components/variations/feature_overrides.h"
#include "third_party/blink/public/common/features.h"

namespace compass {

void RegisterFeatureOverrides(variations::FeatureOverrides& overrides) {
  // A playing, audible video goes into picture-in-picture when its tab is
  // switched away from, whether or not the site asked for it
  // (docs/design/picture-in-picture.md).
  overrides.EnableFeature(
      blink::features::kBrowserInitiatedAutomaticPictureInPicture);

  // No calls to Google that Compass doesn't need (docs/design/privacy.md).
  // The clock check behind certificate errors, over plain HTTP to
  // clients2.google.com.
  overrides.DisableFeature(network_time::kNetworkTimeServiceQuerying);
  // AI Mode eligibility, asked of google.com at startup.
  overrides.DisableFeature(omnibox::kAimServerEligibilityEnabled);
  // Form field type predictions from content-autofill.googleapis.com. Autofill
  // keeps its local heuristics.
  overrides.DisableFeature(
      autofill::features::debug::kAutofillServerCommunication);
  // Gemini in the browser.
  overrides.DisableFeature(features::kGlic);
  // Asks accounts.google.com which Google accounts the cookies hold only when
  // a Google cookie changes, not at launch: there's no browser sign-in.
  overrides.EnableFeature(switches::kAvoidAutoTriggerListAccountsOnStale);

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)
  // No "Continue where you left off: Compass can restore your tabs..." bar:
  // Compass brings its sidebar back itself, and the bar's setting is about
  // upstream's session restore.
  overrides.DisableFeature(features::kSessionRestoreInfobar);
#endif
}

}  // namespace compass
