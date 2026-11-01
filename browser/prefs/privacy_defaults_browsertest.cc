// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Compass's defaults in a real browser: no first run, no profile picker at
// startup, no Google services Compass doesn't keep (docs/design/privacy.md).

#include "base/command_line.h"
#include "base/feature_list.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/profiles/profile_picker.h"
#include "chrome/browser/ui/startup/first_run_service.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/autofill/core/common/autofill_debug_features.h"
#include "components/embedder_support/pref_names.h"
#include "components/network_time/network_time_tracker.h"
#include "components/omnibox/browser/aim_eligibility_service_features.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/signin/public/base/signin_pref_names.h"
#include "components/signin/public/base/signin_switches.h"
#include "components/unified_consent/pref_names.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

// A first run, as on a new install: the test harness's --no-first-run goes.
class PrivacyDefaultsBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->RemoveSwitch(switches::kNoFirstRun);
    command_line->AppendSwitch(switches::kForceFirstRun);
  }
};

IN_PROC_BROWSER_TEST_F(PrivacyDefaultsBrowserTest, NoFirstRunExperience) {
  // A window opened, and no first-run picker is pending.
  ASSERT_TRUE(browser());
  EXPECT_FALSE(ProfilePicker::IsOpen());
  EXPECT_FALSE(
      FirstRunServiceFactory::GetForBrowserContext(browser()->GetProfile()));
}

IN_PROC_BROWSER_TEST_F(PrivacyDefaultsBrowserTest, NoProfilePickerAtStartup) {
  EXPECT_EQ(1, g_browser_process->local_state()->GetInteger(
                   prefs::kBrowserProfilePickerAvailabilityOnStartup));
}

IN_PROC_BROWSER_TEST_F(PrivacyDefaultsBrowserTest, GoogleServicesOff) {
  EXPECT_FALSE(
      base::FeatureList::IsEnabled(network_time::kNetworkTimeServiceQuerying));
  EXPECT_FALSE(
      base::FeatureList::IsEnabled(omnibox::kAimServerEligibilityEnabled));
  EXPECT_FALSE(base::FeatureList::IsEnabled(
      autofill::features::debug::kAutofillServerCommunication));
  EXPECT_FALSE(base::FeatureList::IsEnabled(features::kGlic));
  EXPECT_TRUE(base::FeatureList::IsEnabled(
      switches::kAvoidAutoTriggerListAccountsOnStale));

  PrefService* prefs = browser()->GetProfile()->GetPrefs();
  EXPECT_FALSE(
      prefs->GetBoolean(password_manager::prefs::kPasswordLeakDetectionEnabled));
  EXPECT_FALSE(prefs->GetBoolean(embedder_support::kAlternateErrorPagesEnabled));
  EXPECT_FALSE(prefs->GetBoolean(
      unified_consent::prefs::kUrlKeyedAnonymizedDataCollectionEnabled));
  // No browser sign-in without Google's OAuth client.
  EXPECT_FALSE(prefs->GetBoolean(prefs::kSigninAllowed));
}

}  // namespace
}  // namespace compass
