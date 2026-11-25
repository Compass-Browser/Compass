// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/present_as_safari/present_as_safari.h"

#include "compass/browser/prefs/compass_prefs.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::present_as_safari {
namespace {

TEST(PresentAsSafariTest, SiteIsTheRegistrableDomain) {
  EXPECT_EQ(SiteFor(GURL("https://player.example.com/watch")), "example.com");
  EXPECT_EQ(SiteFor(GURL("https://www.bbc.co.uk/")), "bbc.co.uk");
  EXPECT_FALSE(SiteFor(GURL("https://127.0.0.1/")));
  EXPECT_FALSE(SiteFor(GURL("chrome://settings")));
  EXPECT_FALSE(SiteFor(GURL("file:///tmp/a.html")));
}

TEST(PresentAsSafariTest, EnableForASiteCoversItsSubdomains) {
  sync_preferences::TestingPrefServiceSyncable prefs;
  RegisterCompassProfilePrefs(prefs.registry());

  const GURL watch("https://player.example.com/watch");
  EXPECT_FALSE(IsEnabledFor(&prefs, watch));
  SetEnabledFor(&prefs, watch, true);
  SetEnabledFor(&prefs, watch, true);  // Once.
  EXPECT_EQ(Sites(&prefs), std::vector<std::string>{"example.com"});
  EXPECT_TRUE(IsEnabledFor(&prefs, GURL("https://www.example.com/")));
  EXPECT_FALSE(IsEnabledFor(&prefs, GURL("https://example.org/")));

  SetEnabledFor(&prefs, GURL("https://example.com/"), false);
  EXPECT_FALSE(IsEnabledFor(&prefs, watch));
  EXPECT_TRUE(Sites(&prefs).empty());
}

}  // namespace
}  // namespace compass::present_as_safari
