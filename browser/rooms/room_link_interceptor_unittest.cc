// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/rooms/room_link_interceptor.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::rooms {
namespace {

TEST(RoomLinkInterceptorTest, FindsInviteLinks) {
  EXPECT_EQ(JoinFragmentFromUrl(GURL("https://compassbrowser.org/join#eyJl")),
            "eyJl");
  EXPECT_EQ(JoinFragmentFromUrl(GURL("https://compassbrowser.org/join.html#a-_9")),
            "a-_9");
  EXPECT_EQ(JoinFragmentFromUrl(GURL("chrome://join#abc")), "abc");
  EXPECT_EQ(JoinFragmentFromUrl(GURL("compass://join#abc")), "abc");
  EXPECT_EQ(JoinPageUrlFor("abc"), GURL("chrome://compass-share/#join=abc"));
}

TEST(RoomLinkInterceptorTest, IgnoresOtherLinks) {
  EXPECT_FALSE(JoinFragmentFromUrl(GURL("https://compassbrowser.org/join")));
  EXPECT_FALSE(JoinFragmentFromUrl(GURL("https://compassbrowser.org/join#")));
  EXPECT_FALSE(JoinFragmentFromUrl(GURL("http://compassbrowser.org/join#abc")));
  EXPECT_FALSE(JoinFragmentFromUrl(GURL("https://example.com/join#abc")));
  EXPECT_FALSE(JoinFragmentFromUrl(GURL("https://compassbrowser.org/boost#abc")));
  EXPECT_FALSE(
      JoinFragmentFromUrl(GURL("https://compassbrowser.org/join#a%20b")));
  EXPECT_FALSE(
      JoinFragmentFromUrl(GURL("https://compassbrowser.org/join#a=b")));
}

}  // namespace
}  // namespace compass::rooms
