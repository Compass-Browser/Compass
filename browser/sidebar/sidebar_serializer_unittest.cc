// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/sidebar/sidebar_serializer.h"

#include <algorithm>
#include <optional>
#include <string>

#include "base/json/json_reader.h"
#include "base/strings/utf_string_conversions.h"
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
  // Today isn't saved.
  model.AddTab(model.today_root(), 0, GURL("https://t.test/"), u"T");

  SpaceModel restored;
  EXPECT_EQ(DeserializePinned(SerializePinned(model), restored, 0), 2u);

  EXPECT_EQ(Describe(restored.pinned_root()), "Work{Mail,Docs{Doc}},[A|B]");
  EXPECT_EQ(restored.pinned_root()->children()[0]->children()[0]->url(),
            GURL("https://mail.test/"));
  EXPECT_TRUE(restored.today_root()->children().empty());
}

TEST(SidebarSerializerTest, InsertsAtTheGivenIndex) {
  SpaceModel source;
  source.AddTab(source.pinned_root(), 0, GURL("https://a.test/"), u"A");
  SpaceModel model;
  model.AddTab(model.pinned_root(), 0, GURL("https://x.test/"), u"X");

  DeserializePinned(SerializePinned(source), model, 0);

  EXPECT_EQ(Describe(model.pinned_root()), "A,X");
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
    ]
  })",
                             base::JSON_PARSE_RFC);
  ASSERT_TRUE(data && data->is_dict());
  SpaceModel model;

  EXPECT_EQ(DeserializePinned(data->GetDict(), model, 0), 2u);

  // A split left with one pane is just that pane.
  EXPECT_EQ(Describe(model.pinned_root()), "Kept,Only");
}

TEST(SidebarSerializerTest, IgnoresOtherVersions) {
  std::optional<base::Value> data = base::JSONReader::Read(
      R"({"version": 2, "pinned": [{"type": "tab", "title": "A"}]})",
      base::JSON_PARSE_RFC);
  SpaceModel model;

  EXPECT_EQ(DeserializePinned(data->GetDict(), model, 0), 0u);
  EXPECT_TRUE(model.pinned_root()->children().empty());
}

}  // namespace
}  // namespace strata
