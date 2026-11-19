// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/common/clean_url.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

std::string Clean(const char* url) {
  return RemoveTrackingParams(GURL(url)).spec();
}

TEST(CleanUrlTest, GlobalParams) {
  // MPL2 list.
  EXPECT_EQ(Clean("https://example.com/a?utm_source=x&utm_medium=y"),
            "https://example.com/a");
  EXPECT_EQ(Clean("https://example.com/a?id=3&fbclid=abc&page=2"),
            "https://example.com/a?id=3&page=2");
  // Names compared without case.
  EXPECT_EQ(Clean("https://example.com/?GCLID=1&UTM_Campaign=z&q=a"),
            "https://example.com/?q=a");
  // LGPL list.
  EXPECT_EQ(Clean("https://example.com/?utm_newsletter_id=1&q=a"),
            "https://example.com/?q=a");
  // The fragment stays.
  EXPECT_EQ(Clean("http://example.com/?mc_eid=2#top"),
            "http://example.com/#top");
}

TEST(CleanUrlTest, KeepsEverythingElse) {
  // Order, spelling, empty values and parameters without "=" are kept.
  EXPECT_EQ(Clean("https://example.com/?b=2&a=%20x&flag&e=&utm_source=9"),
            "https://example.com/?b=2&a=%20x&flag&e=");
  EXPECT_EQ(Clean("https://example.com/?utmost=1&source=2"),
            "https://example.com/?utmost=1&source=2");
  EXPECT_EQ(Clean("https://example.com/#utm_source=x"),
            "https://example.com/#utm_source=x");
  const GURL untouched("https://example.com/?q=a");
  EXPECT_EQ(RemoveTrackingParams(untouched), untouched);
}

TEST(CleanUrlTest, SiteParams) {
  // By registrable domain, subdomains too (schemelessSites).
  EXPECT_EQ(Clean("https://open.spotify.com/track/1?si=abc"),
            "https://open.spotify.com/track/1");
  EXPECT_EQ(Clean("https://youtu.be/abc?si=XyZ&t=42"),
            "https://youtu.be/abc?t=42");
  EXPECT_EQ(Clean("https://www.youtube.com/watch?v=abc&si=XyZ"),
            "https://www.youtube.com/watch?v=abc");
  EXPECT_EQ(Clean("https://x.com/a/status/1?s=20&t=abc"),
            "https://x.com/a/status/1?t=abc");
  // Only on their sites.
  EXPECT_EQ(Clean("https://example.com/?si=1&s=2"),
            "https://example.com/?si=1&s=2");
  // An exact host (hosts).
  EXPECT_EQ(Clean("https://www.amazon.com.be/dp/1?pd_rd_i=x&k=y"),
            "https://www.amazon.com.be/dp/1?k=y");
}

TEST(CleanUrlTest, NestedUrls) {
  EXPECT_EQ(
      Clean("https://example.com/r?to=https%3A%2F%2Fa.example%2F%3Fid%3D1%26"
            "utm_source%3Dx&utm_medium=y"),
      "https://example.com/r?to=https%3A%2F%2Fa.example%2F%3Fid%3D1");
  // A value that isn't a URL, or a URL with nothing to clean, is kept as is.
  EXPECT_EQ(Clean("https://example.com/r?to=https%3A%2F%2Fa.example%2F"),
            "https://example.com/r?to=https%3A%2F%2Fa.example%2F");
}

TEST(CleanUrlTest, OnlyWebUrls) {
  EXPECT_EQ(Clean("chrome://settings/?utm_source=x"),
            "chrome://settings/?utm_source=x");
  EXPECT_EQ(Clean("file:///tmp/a.html?utm_source=x"),
            "file:///tmp/a.html?utm_source=x");
  EXPECT_EQ(RemoveTrackingParams(GURL()), GURL());
}

}  // namespace
}  // namespace compass
