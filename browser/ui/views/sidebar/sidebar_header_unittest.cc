// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/sidebar/sidebar_header.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

TEST(SidebarHeaderTest, PillText) {
  EXPECT_EQ(SidebarHeader::GetPillText(GURL("https://www.example.com/a?b")),
            u"example.com");
  EXPECT_EQ(SidebarHeader::GetPillText(GURL("http://sub.example.com:8080/")),
            u"sub.example.com");
  EXPECT_EQ(SidebarHeader::GetPillText(GURL("chrome://newtab/")),
            u"Search or enter URL");
  EXPECT_EQ(SidebarHeader::GetPillText(GURL("about:blank")),
            u"Search or enter URL");
  EXPECT_EQ(SidebarHeader::GetPillText(GURL()), u"Search or enter URL");
  EXPECT_EQ(SidebarHeader::GetPillText(GURL("chrome://settings/")),
            u"chrome://settings/");
  EXPECT_EQ(SidebarHeader::GetPillText(GURL("file:///tmp/a.html")),
            u"file:///tmp/a.html");
}

}  // namespace
}  // namespace compass
