// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/prefs/pref_defaults.h"

#include <utility>

#include "base/values.h"
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

// The upstream prefs OverrideProfilePrefDefaults() changes on every platform.
void RegisterUpstreamProfilePrefs(TestingPrefServiceSimple& prefs) {
  prefs.registry()->RegisterBooleanPref(
      password_manager::prefs::kPasswordLeakDetectionEnabled, true);
  prefs.registry()->RegisterBooleanPref(
      embedder_support::kAlternateErrorPagesEnabled, true);
}

TEST(PrefDefaultsTest, DownloadsButtonPinned) {
  TestingPrefServiceSimple prefs;
  RegisterUpstreamProfilePrefs(prefs);
  base::ListValue upstream;
  upstream.Append("kActionShowChromeLabs");
  prefs.registry()->RegisterListPref("toolbar.pinned_actions",
                                     std::move(upstream));
  OverrideProfilePrefDefaults(prefs.registry());
  const base::ListValue& pinned = prefs.GetList("toolbar.pinned_actions");
  ASSERT_EQ(2u, pinned.size());
  EXPECT_EQ("kActionShowChromeLabs", pinned[0].GetString());
  EXPECT_EQ("kActionShowDownloads", pinned[1].GetString());
}

TEST(PrefDefaultsTest, NoPinnedActionsWithoutTheToolbar) {
  // Android registers no toolbar prefs; nothing is added.
  TestingPrefServiceSimple prefs;
  RegisterUpstreamProfilePrefs(prefs);
  OverrideProfilePrefDefaults(prefs.registry());
  EXPECT_FALSE(prefs.FindPreference("toolbar.pinned_actions"));
}

}  // namespace
}  // namespace compass
