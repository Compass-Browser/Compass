// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/feature_overrides.h"

#include "build/build_config.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/common/chrome_features.h"
#include "components/autofill/core/common/autofill_debug_features.h"
#include "components/lens/lens_features.h"
#include "components/network_time/network_time_tracker.h"
#include "components/omnibox/browser/aim_eligibility_service_features.h"
#include "components/on_device_translation/buildflags/buildflags.h"
#include "components/signin/public/base/signin_switches.h"
#include "components/variations/feature_overrides.h"
#include "media/base/media_switches.h"
#include "third_party/blink/public/common/features.h"

#if BUILDFLAG(IS_ANDROID)
#include "chrome/browser/flags/android/chrome_feature_list.h"
#endif

#if BUILDFLAG(ENABLE_ON_DEVICE_TRANSLATION)
#include "base/command_line.h"
#include "compass/browser/translate/translatekit_library.h"
#endif

namespace compass {

void RegisterFeatureOverrides(variations::FeatureOverrides& overrides) {
  // A playing, audible video goes into picture-in-picture when its tab is
  // switched away from, whether or not the site asked for it
  // (docs/design/picture-in-picture.md).
  overrides.EnableFeature(
      blink::features::kBrowserInitiatedAutomaticPictureInPicture);
#if BUILDFLAG(IS_ANDROID)
  // Android's tab helper for it (TabFeatures), which follows the tab models;
  // leaving the app goes through //compass/browser/android's PipBridge
  // (docs/design/android.md, "Auto picture-in-picture").
  overrides.EnableFeature(media::kAutoPictureInPictureAndroid);
#else
  // ...and when its window is covered or minimized, or Compass is left for
  // another app, unless the user turned that off (prefs::kPipOnLeavingWindow,
  // checked in the auto_picture_in_picture_tab_helper.cc patch).
  overrides.EnableFeature(media::kAutoPictureInPictureOnWindowOccluded);
#endif

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
#if !BUILDFLAG(IS_ANDROID)
  // Google Lens: the app menu's "Search this tab with Image Search", the page
  // menu's "Search with Google Lens" and "Search image with Google Lens"
  // (an image keeps "Search image with <engine>").
  overrides.DisableFeature(lens::features::kLensOverlay);
  overrides.DisableFeature(lens::features::kLensStandalone);
#endif
  // Asks accounts.google.com which Google accounts the cookies hold only when
  // a Google cookie changes, not at launch: there's no browser sign-in.
  overrides.EnableFeature(switches::kAvoidAutoTriggerListAccountsOnStale);

#if BUILDFLAG(IS_ANDROID)
  // Compass's own bar is the bottom bar (docs/design/android.md); Chrome's,
  // with the reworked top toolbar that comes with it, stays off. Builds that
  // aren't official turn it on as a test default.
  overrides.DisableFeature(chrome::android::kAndroidBottomBar);
#endif

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)
  // No "Continue where you left off: Compass can restore your tabs..." bar:
  // Compass brings its sidebar back itself, and the bar's setting is about
  // upstream's session restore.
  overrides.DisableFeature(features::kSessionRestoreInfobar);
#endif

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  // No "Make Compass your default PDF viewer?" infobar, at startup or on a
  // PDF: Compass doesn't nag about defaults (the default browser prompt is
  // off too, chromium_src/.../default_browser_prompt.cc).
  overrides.DisableFeature(features::kPdfInfoBar);
#endif

#if BUILDFLAG(ENABLE_ON_DEVICE_TRANSLATION)
  // On-device translation runs Compass's own library, not Google's
  // TranslateKit component (docs/design/translation.md). Not a feature, but
  // this runs early in the browser process, before anything translates.
  UseBundledTranslateKit(*base::CommandLine::ForCurrentProcess());
#endif
}

}  // namespace compass
