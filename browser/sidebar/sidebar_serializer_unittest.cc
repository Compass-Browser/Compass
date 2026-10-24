// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/sidebar/sidebar_serializer.h"

#include <algorithm>
#include <optional>
#include <string>

#include "base/json/json_reader.h"
#include "base/strings/utf_string_conversions.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace strata {
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
      {"type": "folder", "title": "No folders in Today"},
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
  list.SetActiveSpace(*work);

  SpaceList restored;
  EXPECT_EQ(DeserializeSpaceList(SerializeSpaceList(list), restored), 3u);

  EXPECT_EQ(Names(restored), "Home,[Work],Play");
  // The active space took over the saved one's identity.
  EXPECT_EQ(restored.active_space().id(), work->id());
  EXPECT_EQ(Describe(restored.active_space().pinned_root()), "W");
  EXPECT_EQ(Describe(restored.FindSpace(play->id())->today_root()), "P");
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

}  // namespace
}  // namespace strata
