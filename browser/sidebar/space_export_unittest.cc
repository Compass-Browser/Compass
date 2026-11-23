// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/space_export.h"

#include <string>

#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

// The lines after the header, without the last "</DL><p>".
std::string Body(const std::string& html) {
  const std::string open = "<H1>Bookmarks</H1>\n<DL><p>\n";
  const size_t start = html.find(open);
  EXPECT_NE(start, std::string::npos);
  std::string body = html.substr(start + open.size());
  const std::string close = "</DL><p>\n";
  EXPECT_TRUE(body.ends_with(close));
  body.resize(body.size() - close.size());
  return body;
}

TEST(SpaceExportTest, HeaderOnlyWithNoSpaces) {
  const std::string html = SpacesToBookmarksHtml(SpaceExport());
  EXPECT_TRUE(html.starts_with("<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"));
  EXPECT_EQ(Body(html), "");
}

TEST(SpaceExportTest, PinnedFoldersSplitsAndToday) {
  SpaceModel space;
  space.SetName(u"Work & <Stuff>");
  SidebarNode* pinned = space.pinned_root();
  SidebarNode* docs = space.AddFolder(pinned, 0, u"Docs");
  space.AddTab(docs, 0, GURL("https://docs.example.com/a?x=1&y=2"), u"A");
  // A pinned tab that has navigated away is still its own page.
  SidebarNode* mail =
      space.AddTab(pinned, 1, GURL("https://mail.example.com/"), u"Mail");
  space.SetUrl(mail, GURL("https://mail.example.com/thread/42"));
  // The browser's own pages don't leave Compass.
  space.AddTab(pinned, 2, GURL("chrome://settings/"), u"Settings");

  SidebarNode* today = space.today_root();
  SidebarNode* split = space.AddSplit(today, 0);
  space.AddTab(split, 0, GURL("https://left.example.com/"), u"Left");
  space.AddTab(split, 1, GURL("https://right.example.com/"), u"");

  SpaceExport input;
  input.spaces.push_back(&space);
  EXPECT_EQ(
      Body(SpacesToBookmarksHtml(input)),
      "    <DT><H3>Work &amp; &lt;Stuff&gt;</H3>\n"
      "    <DL><p>\n"
      "        <DT><H3>Docs</H3>\n"
      "        <DL><p>\n"
      "            <DT><A HREF=\"https://docs.example.com/a?x=1&amp;y=2\">"
      "A</A>\n"
      "        </DL><p>\n"
      "        <DT><A HREF=\"https://mail.example.com/\">Mail</A>\n"
      "        <DT><H3>Today</H3>\n"
      "        <DL><p>\n"
      "            <DT><A HREF=\"https://left.example.com/\">Left</A>\n"
      "            <DT><A HREF=\"https://right.example.com/\">"
      "https://right.example.com/</A>\n"
      "        </DL><p>\n"
      "    </DL><p>\n");
}

TEST(SpaceExportTest, FavoritesFirst) {
  SpaceModel favorites(SpaceModel::Kind::kFavorites);
  favorites.AddTab(favorites.pinned_root(), 0,
                   GURL("https://calendar.example.com/"), u"Calendar");
  SpaceModel empty_favorites(SpaceModel::Kind::kFavorites);
  SpaceModel home;
  home.SetName(u"Home");

  SpaceExport input;
  input.favorites = {&favorites, &empty_favorites};
  input.spaces.push_back(&home);
  EXPECT_EQ(
      Body(SpacesToBookmarksHtml(input)),
      "    <DT><H3>Favorites</H3>\n"
      "    <DL><p>\n"
      "        <DT><A HREF=\"https://calendar.example.com/\">Calendar</A>\n"
      "    </DL><p>\n"
      "    <DT><H3>Home</H3>\n"
      "    <DL><p>\n"
      "    </DL><p>\n");
}

}  // namespace
}  // namespace compass
