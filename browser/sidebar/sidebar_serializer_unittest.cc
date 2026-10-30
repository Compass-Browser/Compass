// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/sidebar_serializer.h"

#include <algorithm>
#include <optional>
#include <string>

#include "base/json/json_reader.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

using Type = SidebarNode::Type;

// |parent|'s subtree as text: tabs by title, "Folder{...}", "[pane|pane]".
std::string Describe(const SidebarNode* parent) {
  std::string result;
  for (const auto& child : parent->children()) {
    if (!result.empty()) {
      result += ",";
    }
    switch (child->type()) {
      case Type::kTab:
        result += base::UTF16ToUTF8(child->title());
        break;
      case Type::kFolder:
        result += base::UTF16ToUTF8(child->title()) + "{" +
                  Describe(child.get()) + "}";
        break;
      case Type::kSplit: {
        std::string panes = Describe(child.get());
        std::replace(panes.begin(), panes.end(), ',', '|');
        result += "[" + panes + "]";
        break;
      }
      case Type::kRoot:
        break;
    }
  }
  return result;
}

TEST(SidebarSerializerTest, RoundTripsThePinnedTree) {
  SpaceModel model;
  SidebarNode* pinned = model.pinned_root();
  SidebarNode* work = model.AddFolder(pinned, 0, u"Work");
  model.AddTab(work, 0, GURL("https://mail.test/"), u"Mail");
  SidebarNode* inner = model.AddFolder(work, 1, u"Docs");
  model.AddTab(inner, 0, GURL("https://docs.test/"), u"Doc");
  SidebarNode* a = model.AddTab(pinned, 1, GURL("https://a.test/"), u"A");
  SidebarNode* b = model.AddTab(pinned, 2, GURL("https://b.test/"), u"B");
  model.Split(a, b);

  SpaceModel restored;
  EXPECT_EQ(DeserializeSpace(SerializeSpace(model), restored), 2u);

  EXPECT_EQ(Describe(restored.pinned_root()), "Work{Mail,Docs{Doc}},[A|B]");
  EXPECT_EQ(restored.pinned_root()->children()[0]->children()[0]->url(),
            GURL("https://mail.test/"));
  EXPECT_TRUE(restored.today_root()->children().empty());
}

TEST(SidebarSerializerTest, RoundTripsFolderCollapseState) {
  SpaceModel model;
  SidebarNode* work = model.AddFolder(model.pinned_root(), 0, u"Work");
  SidebarNode* docs = model.AddFolder(work, 0, u"Docs");
  model.AddFolder(model.pinned_root(), 1, u"Play");
  model.SetCollapsed(docs, true);
  model.SetCollapsed(work, true);

  SpaceModel restored;
  DeserializeSpace(SerializeSpace(model), restored);

  ASSERT_EQ(Describe(restored.pinned_root()), "Work{Docs{}},Play{}");
  const SidebarNode* restored_work = restored.pinned_root()->children()[0].get();
  EXPECT_TRUE(restored_work->collapsed());
  EXPECT_TRUE(restored_work->children()[0]->collapsed());
  EXPECT_FALSE(restored.pinned_root()->children()[1]->collapsed());
}

TEST(SidebarSerializerTest, RoundTripsCustomTitles) {
  SpaceModel model;
  SidebarNode* pinned = model.pinned_root();
  SidebarNode* mail =
      model.AddTab(pinned, 0, GURL("https://mail.test/"), u"Mail (3)");
  SidebarNode* a = model.AddTab(pinned, 1, GURL("https://a.test/"), u"A");
  SidebarNode* b = model.AddTab(pinned, 2, GURL("https://b.test/"), u"B");
  model.Split(a, b);
  model.SetCustomTitle(mail, u"Inbox");
  model.SetCustomTitle(b, u"Bee");

  SpaceModel restored;
  DeserializeSpace(SerializeSpace(model), restored);

  ASSERT_EQ(Describe(restored.pinned_root()), "Mail (3),[A|B]");
  const auto& entries = restored.pinned_root()->children();
  EXPECT_EQ(entries[0]->custom_title(), u"Inbox");
  EXPECT_TRUE(entries[1]->children()[0]->custom_title().empty());
  EXPECT_EQ(entries[1]->children()[1]->custom_title(), u"Bee");
}

TEST(SidebarSerializerTest, RoundTripsTodayFolders) {
  SpaceModel model;
  SidebarNode* trip = model.AddFolder(model.today_root(), 0, u"Trip");
  model.AddTab(trip, 0, GURL("https://map.test/"), u"Map");
  // A new tab page isn't saved, but its folder is.
  SidebarNode* empty = model.AddFolder(model.today_root(), 1, u"Later");
  model.AddTab(empty, 0, GURL("chrome://newtab/"), u"New Tab");
  model.SetCollapsed(trip, true);

  SpaceModel restored;
  DeserializeSpace(SerializeSpace(model), restored);

  EXPECT_EQ(Describe(restored.today_root()), "Trip{Map},Later{}");
  EXPECT_TRUE(restored.today_root()->children()[0]->collapsed());
}

TEST(SidebarSerializerTest, SavesTodayWithoutNewTabPages) {
  SpaceModel model;
  SidebarNode* today = model.today_root();
  model.AddTab(today, 0, GURL("https://t.test/"), u"T");
  model.AddTab(today, 1, GURL("chrome://newtab/"), u"New Tab");
  SidebarNode* a = model.AddTab(today, 2, GURL("https://a.test/"), u"A");
  SidebarNode* b = model.AddTab(today, 3, GURL("https://b.test/"), u"B");
  model.Split(a, b);
  SidebarNode* c = model.AddTab(today, 3, GURL("https://c.test/"), u"C");
  SidebarNode* blank = model.AddTab(today, 4, GURL("about:blank"), u"Blank");
  model.Split(c, blank);

  SpaceModel restored;
  EXPECT_EQ(DeserializeSpace(SerializeSpace(model), restored), 3u);

  // A split left with one pane is just that pane.
  EXPECT_EQ(Describe(restored.today_root()), "T,[A|B],C");
  EXPECT_TRUE(restored.pinned_root()->children().empty());
}

TEST(SidebarSerializerTest, InsertsAheadOfExistingEntries) {
  SpaceModel source;
  source.AddTab(source.pinned_root(), 0, GURL("https://a.test/"), u"A");
  source.AddTab(source.today_root(), 0, GURL("https://b.test/"), u"B");
  SpaceModel model;
  model.AddTab(model.pinned_root(), 0, GURL("https://x.test/"), u"X");
  model.AddTab(model.today_root(), 0, GURL("https://y.test/"), u"Y");

  DeserializeSpace(SerializeSpace(source), model);

  EXPECT_EQ(Describe(model.pinned_root()), "A,X");
  EXPECT_EQ(Describe(model.today_root()), "B,Y");
}

TEST(SidebarSerializerTest, SkipsWhatTheModelCantHold) {
  std::optional<base::Value> data =
      base::JSONReader::Read(R"({
    "version": 1,
    "pinned": [
      {"type": "tab", "title": "Kept", "url": "https://k.test/"},
      {"type": "space-station"},
      "not a node",
      {"type": "split", "children": [
        {"type": "folder", "title": "No folders in splits"},
        {"type": "tab", "title": "Only"}
      ]}
    ],
    "today": [
      {"type": "tab", "title": "T", "url": "https://t.test/"}
    ]
  })",
                             base::JSON_PARSE_RFC);
  ASSERT_TRUE(data && data->is_dict());
  SpaceModel model;

  EXPECT_EQ(DeserializeSpace(data->GetDict(), model), 3u);

  // A split left with one pane is just that pane.
  EXPECT_EQ(Describe(model.pinned_root()), "Kept,Only");
  EXPECT_EQ(Describe(model.today_root()), "T");
}

TEST(SidebarSerializerTest, IgnoresOtherVersions) {
  std::optional<base::Value> data = base::JSONReader::Read(
      R"({"version": 3, "pinned": [{"type": "tab", "title": "A"}]})",
      base::JSON_PARSE_RFC);
  SpaceList list;

  EXPECT_EQ(DeserializeSpaceList(data->GetDict(), list), 0u);
  EXPECT_EQ(list.spaces().size(), 1u);
  EXPECT_TRUE(list.active_space().pinned_root()->children().empty());
}

// Names of |list|'s spaces, with the active one in brackets.
std::string Names(const SpaceList& list) {
  std::string names;
  for (const auto& space : list.spaces()) {
    std::string name = base::UTF16ToUTF8(space->name());
    if (space.get() == &list.active_space()) {
      name = "[" + name + "]";
    }
    names += (names.empty() ? "" : ",") + name;
  }
  return names;
}

TEST(SidebarSerializerTest, RoundTripsTheSpaceList) {
  SpaceList list;
  list.active_space().SetName(u"Home");
  SpaceModel* work = list.AddSpace(1, u"Work");
  SpaceModel* play = list.AddSpace(2, u"Play");
  work->AddTab(work->pinned_root(), 0, GURL("https://w.test/"), u"W");
  play->AddTab(play->today_root(), 0, GURL("https://p.test/"), u"P");
  work->SetColor(SpaceColor::kGreen);
  play->SetColor(SpaceColor::kPink);
  list.SetActiveSpace(*work);

  SpaceList restored;
  EXPECT_EQ(DeserializeSpaceList(SerializeSpaceList(list), restored), 3u);

  EXPECT_EQ(Names(restored), "Home,[Work],Play");
  // The active space took over the saved one's identity.
  EXPECT_EQ(restored.active_space().id(), work->id());
  EXPECT_EQ(Describe(restored.active_space().pinned_root()), "W");
  EXPECT_EQ(Describe(restored.FindSpace(play->id())->today_root()), "P");
  EXPECT_EQ(restored.active_space().color(), SpaceColor::kGreen);
  EXPECT_EQ(restored.FindSpace(play->id())->color(), SpaceColor::kPink);
  EXPECT_EQ(restored.spaces()[0]->color(), SpaceColor::kGrey);
}

TEST(SidebarSerializerTest, RoundTripsFavorites) {
  SpaceList list;
  SpaceModel& favorites = list.favorites();
  favorites.AddTab(favorites.pinned_root(), 0, GURL("https://a.test/"), u"A");
  favorites.AddTab(favorites.pinned_root(), 1, GURL("https://b.test/"), u"B");

  SpaceList restored;
  // Loaded ahead of one added before the load.
  restored.favorites().AddTab(restored.favorites().pinned_root(), 0,
                              GURL("https://early.test/"), u"Early");
  DeserializeSpaceList(SerializeSpaceList(list), restored);

  EXPECT_EQ(Describe(restored.favorites().pinned_root()), "A,B,Early");
  EXPECT_EQ(restored.favorites().pinned_root()->children()[1]->url(),
            GURL("https://b.test/"));
  EXPECT_EQ(Describe(restored.active_space().pinned_root()), "");
}

TEST(SidebarSerializerTest, FavoritesSkipAllButTabs) {
  std::optional<base::Value> data = base::JSONReader::Read(
      R"({"version": 2, "spaces": [{"name": "Home"}], "favorites": [
            {"type": "folder", "title": "F", "children": []},
            {"type": "tab", "title": "A", "url": "https://a.test/"},
            {"type": "split", "children": [
              {"type": "tab", "title": "B", "url": "https://b.test/"}]},
            "junk"]})",
      base::JSON_PARSE_RFC);
  ASSERT_TRUE(data);
  SpaceList list;
  DeserializeSpaceList(data->GetDict(), list);
  EXPECT_EQ(Describe(list.favorites().pinned_root()), "A");
}

TEST(SidebarSerializerTest, OlderFilesHaveNoFavorites) {
  std::optional<base::Value> data = base::JSONReader::Read(
      R"({"version": 2, "spaces": [{"name": "Home", "pinned": [
            {"type": "tab", "title": "P", "url": "https://p.test/"}]}]})",
      base::JSON_PARSE_RFC);
  ASSERT_TRUE(data);
  SpaceList list;
  EXPECT_EQ(DeserializeSpaceList(data->GetDict(), list), 1u);
  EXPECT_TRUE(list.favorites().pinned_root()->children().empty());
  EXPECT_EQ(Describe(list.active_space().pinned_root()), "P");
}

TEST(SidebarSerializerTest, TheActiveSpaceKeepsWhatItHadBeforeTheLoad) {
  SpaceList saved;
  saved.active_space().SetName(u"Home");
  saved.active_space().AddTab(saved.active_space().today_root(), 0,
                              GURL("https://saved.test/"), u"Saved");
  SpaceList list;
  // A window opened before the load.
  list.active_space().AddTab(list.active_space().today_root(), 0,
                             GURL("https://early.test/"), u"Early");

  DeserializeSpaceList(SerializeSpaceList(saved), list);

  EXPECT_EQ(Names(list), "[Home]");
  EXPECT_EQ(Describe(list.active_space().today_root()), "Saved,Early");
}

TEST(SidebarSerializerTest, ReadsVersion1AsOneSpace) {
  std::optional<base::Value> data = base::JSONReader::Read(
      R"({"version": 1,
          "pinned": [{"type": "tab", "title": "A", "url": "https://a.test/"}],
          "today": [{"type": "tab", "title": "T", "url": "https://t.test/"}]})",
      base::JSON_PARSE_RFC);
  SpaceList list;

  EXPECT_EQ(DeserializeSpaceList(data->GetDict(), list), 1u);

  EXPECT_EQ(list.spaces().size(), 1u);
  EXPECT_EQ(Describe(list.active_space().pinned_root()), "A");
  EXPECT_EQ(Describe(list.active_space().today_root()), "T");
}

TEST(SidebarSerializerTest, RoundTripsArchiveState) {
  SpaceList list;
  SpaceModel& model = list.active_space();
  const base::Time viewed = base::Time::Now() - base::Hours(3);
  SidebarNode* tab =
      model.AddTab(model.today_root(), 0, GURL("https://t.test/"), u"T");
  model.SetLastActive(tab, viewed);
  SidebarNode* a = model.AddTab(model.today_root(), 1, GURL("https://a.test/"),
                                u"A");
  SidebarNode* b = model.AddTab(model.today_root(), 2, GURL("https://b.test/"),
                                u"B");
  model.Split(a, b);
  const base::Time archived = base::Time::Now();
  model.Archive(model.today_root()->children()[1].get(), archived);
  model.SetArchiveAfter(base::Days(7));

  SpaceList restored;
  DeserializeSpaceList(SerializeSpaceList(list), restored);

  SpaceModel& space = restored.active_space();
  EXPECT_EQ(space.archive_after(), base::Days(7));
  ASSERT_EQ(Describe(space.today_root()), "T");
  EXPECT_EQ(space.today_root()->children()[0]->last_active(), viewed);
  ASSERT_EQ(space.archive().size(), 1u);
  EXPECT_EQ(space.archive()[0].archived_at, archived);
  ASSERT_EQ(space.archive()[0].tabs.size(), 2u);
  EXPECT_EQ(space.archive()[0].tabs[1].title, u"B");
  EXPECT_EQ(space.archive()[0].tabs[1].url, GURL("https://b.test/"));
}

TEST(SidebarSerializerTest, OlderFilesGetTheDefaultArchiveDelay) {
  std::optional<base::Value> data = base::JSONReader::Read(
      R"({"version": 2, "spaces": [{"name": "Home", "archive_after_hours": 0},
                                   {"name": "Work"}]})",
      base::JSON_PARSE_RFC);
  SpaceList list;

  EXPECT_EQ(DeserializeSpaceList(data->GetDict(), list), 2u);

  EXPECT_EQ(list.spaces()[0]->archive_after(), base::TimeDelta());
  EXPECT_EQ(list.spaces()[1]->archive_after(), kDefaultArchiveAfter);
}

}  // namespace
}  // namespace compass
