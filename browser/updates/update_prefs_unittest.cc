// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/updates/update_prefs.h"

#include "base/version.h"
#include "compass/browser/updates/update_hooks.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::updates {
namespace {

TEST(UpdatePrefsTest, StableByDefault) {
  TestingPrefServiceSimple prefs;
  RegisterUpdatePrefs(prefs.registry());
  EXPECT_EQ(kChannelStable, prefs.GetString(kUpdateChannelPref));
  EXPECT_FALSE(IsBetaChannel(prefs));
}

TEST(UpdatePrefsTest, BetaOnlyWhenPicked) {
  TestingPrefServiceSimple prefs;
  RegisterUpdatePrefs(prefs.registry());
  prefs.SetString(kUpdateChannelPref, kChannelBeta);
  EXPECT_TRUE(IsBetaChannel(prefs));
  prefs.SetString(kUpdateChannelPref, "nightly");
  EXPECT_FALSE(IsBetaChannel(prefs));
}

TEST(UpdateHooksTest, PendingVersion) {
  EXPECT_EQ(nullptr, GetUpdater());
  SetPendingVersion(base::Version("156.0.8078.48.1"));
  ASSERT_TRUE(PendingVersion().has_value());
  // Higher than the running Chromium version, so upstream's poller reports an
  // update even when only Compass changed.
  EXPECT_GT(*PendingVersion(), base::Version("156.0.8078.48"));
}

}  // namespace
}  // namespace compass::updates
