// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/prefs/pref_defaults.h"

#include <algorithm>
#include <utility>

#include "base/values.h"
#include "chrome/common/pref_names.h"
#include "components/embedder_support/pref_names.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_store.h"

namespace compass {

namespace {

// toolbar::prefs::kPinnedActions (chrome/browser/ui/toolbar/
// toolbar_pref_names.h), and the downloads button's action
// (kActionShowDownloads), as the list stores it. Desktop only: Android
// doesn't register it.
constexpr char kPinnedActions[] = "toolbar.pinned_actions";
constexpr char kShowDownloadsAction[] = "kActionShowDownloads";

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
  // The downloads button stays in the sidebar's header, also with no recent
  // downloads: its bubble lists the download folder and the screenshots
  // (docs/design/downloads-bubble.md). Unpinning it from its menu brings
  // back upstream's button that comes and goes.
  const base::Value* pinned = nullptr;
  if (registry->defaults()->GetValue(kPinnedActions, &pinned) &&
      pinned->is_list()) {
    base::ListValue actions = pinned->GetList().Clone();
    if (std::ranges::find(actions, base::Value(kShowDownloadsAction)) ==
        actions.end()) {
      actions.Append(kShowDownloadsAction);
    }
    registry->SetDefaultPrefValue(kPinnedActions,
                                  base::Value(std::move(actions)));
  }
}

}  // namespace compass
