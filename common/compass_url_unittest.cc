// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/common/compass_url.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

TEST(CompassUrlTest, CompassIsAStandardScheme) {
  // Registered by ChromeContentClient (unit tests run with it).
  const GURL url("compass://settings/help?x#y");
  EXPECT_EQ(url.host(), "settings");
  EXPECT_EQ(url.path(), "/help");
}

TEST(CompassUrlTest, ToChromeURL) {
  EXPECT_EQ(ToChromeURL(GURL("compass://settings/help?x#y")),
            GURL("chrome://settings/help?x#y"));
  EXPECT_EQ(ToChromeURL(GURL("COMPASS://newtab")), GURL("chrome://newtab/"));
  EXPECT_EQ(ToChromeURL(GURL("https://compass.example/")),
            GURL("https://compass.example/"));
  EXPECT_EQ(ToChromeURL(GURL("chrome://settings/")),
            GURL("chrome://settings/"));
  EXPECT_EQ(ToChromeURL(GURL()), GURL());
}

TEST(CompassUrlTest, ToCompassURL) {
  EXPECT_EQ(ToCompassURL(GURL("chrome://settings/help?x#y")),
            GURL("compass://settings/help?x#y"));
  EXPECT_EQ(ToCompassURL(GURL("chrome-untrusted://a/")),
            GURL("chrome-untrusted://a/"));
  EXPECT_EQ(ToCompassURL(GURL("https://example.com/")),
            GURL("https://example.com/"));
}

TEST(CompassUrlTest, ToCompassText) {
  EXPECT_EQ(ToCompassText(u"chrome://settings"), u"compass://settings");
  EXPECT_EQ(ToCompassText(u"Chrome://flags/#x"), u"compass://flags/#x");
  EXPECT_EQ(ToCompassText(u"example.com/chrome://"), u"example.com/chrome://");
  EXPECT_EQ(ToCompassText(u"chrome:/"), u"chrome:/");
  EXPECT_EQ(ToCompassText(u""), u"");
}

}  // namespace
}  // namespace compass
