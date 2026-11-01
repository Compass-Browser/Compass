// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/prefs/pref_defaults.h"

#include "chrome/common/pref_names.h"
#include "components/embedder_support/pref_names.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

TEST(PrefDefaultsTest, NoProfilePickerAtStartup) {
  TestingPrefServiceSimple prefs;
  prefs.registry()->RegisterIntegerPref(
      prefs::kBrowserProfilePickerAvailabilityOnStartup, 0);
  OverrideLocalStatePrefDefaults(prefs.registry());
  EXPECT_EQ(1,
            prefs.GetInteger(prefs::kBrowserProfilePickerAvailabilityOnStartup));
}

TEST(PrefDefaultsTest, NoGoogleBackedChecks) {
  TestingPrefServiceSimple prefs;
  prefs.registry()->RegisterBooleanPref(
      password_manager::prefs::kPasswordLeakDetectionEnabled, true);
  prefs.registry()->RegisterBooleanPref(
      embedder_support::kAlternateErrorPagesEnabled, true);
  OverrideProfilePrefDefaults(prefs.registry());
  EXPECT_FALSE(
      prefs.GetBoolean(password_manager::prefs::kPasswordLeakDetectionEnabled));
  EXPECT_FALSE(prefs.GetBoolean(embedder_support::kAlternateErrorPagesEnabled));

  // The user's choice still wins.
  prefs.SetBoolean(embedder_support::kAlternateErrorPagesEnabled, true);
  EXPECT_TRUE(prefs.GetBoolean(embedder_support::kAlternateErrorPagesEnabled));
}

}  // namespace
}  // namespace compass
