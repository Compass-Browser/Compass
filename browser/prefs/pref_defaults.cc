// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/prefs/pref_defaults.h"

#include "base/values.h"
#include "chrome/common/pref_names.h"
#include "components/embedder_support/pref_names.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/prefs/pref_registry_simple.h"

namespace compass {

namespace {

// ProfilePicker::AvailabilityOnStartup::kDisabled, as the pref stores it
// (chrome/browser/ui/profiles/profile_picker.cc, GetAvailabilityOnStartup()).
constexpr int kProfilePickerDisabledOnStartup = 1;

}  // namespace

void OverrideLocalStatePrefDefaults(PrefRegistrySimple* registry) {
  // Several profiles still open straight into a window, with the first space
  // (docs/design/spaces.md, "Startup with several profiles"). Also hides the
  // picker's "Ask on startup" checkbox.
  registry->SetDefaultPrefValue(
      prefs::kBrowserProfilePickerAvailabilityOnStartup,
      base::Value(kProfilePickerDisabledOnStartup));
}

void OverrideProfilePrefDefaults(PrefRegistrySimple* registry) {
  // Checks submitted passwords against Google's leak database: without a
  // Google account and API key it can't work, and it still sends requests.
  registry->SetDefaultPrefValue(
      password_manager::prefs::kPasswordLeakDetectionEnabled,
      base::Value(false));
  // Error pages ask Google Public DNS and gstatic.com what went wrong.
  registry->SetDefaultPrefValue(embedder_support::kAlternateErrorPagesEnabled,
                                base::Value(false));
}

}  // namespace compass
