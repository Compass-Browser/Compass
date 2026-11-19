// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/prefs/copy_link.h"

#include "compass/browser/prefs/compass_prefs.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

TEST(CopyLinkTest, FollowsThePref) {
  TestingPrefServiceSimple prefs;
  RegisterCompassProfilePrefs(prefs.registry());
  const GURL url("https://example.com/?id=1&utm_source=x");

  // On by default.
  EXPECT_EQ(LinkToCopy(&prefs, url), GURL("https://example.com/?id=1"));

  prefs.SetBoolean(prefs::kRemoveTrackingFromCopiedLinks, false);
  EXPECT_EQ(LinkToCopy(&prefs, url), url);

  EXPECT_EQ(LinkToCopy(nullptr, url), url);
}

TEST(CopyLinkTest, CompassScheme) {
  TestingPrefServiceSimple prefs;
  RegisterCompassProfilePrefs(prefs.registry());
  EXPECT_EQ(LinkToCopy(&prefs, GURL("chrome://settings/")),
            GURL("compass://settings/"));
  prefs.SetBoolean(prefs::kRemoveTrackingFromCopiedLinks, false);
  EXPECT_EQ(LinkToCopy(&prefs, GURL("chrome://settings/")),
            GURL("compass://settings/"));
}

}  // namespace
}  // namespace compass
