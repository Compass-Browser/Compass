// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/link_folders.h"

#include <string>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

using ::testing::ElementsAre;
using Type = SidebarNode::Type;
using NameSource = SidebarNode::NameSource;

// Titles of |parent|'s children: folders as "Name{child,…}", splits as
// "[pane|pane]".
std::string Tree(const SidebarNode* parent);

std::string Node(const SidebarNode& node) {
  switch (node.type()) {
    case Type::kFolder:
      return base::UTF16ToUTF8(node.title()) + "{" + Tree(&node) + "}";
    case Type::kSplit:
      return "[" + Tree(&node) + "]";
    default:
      return base::UTF16ToUTF8(node.title());
  }
}

std::string Tree(const SidebarNode* parent) {
  std::string tree;
  for (const auto& child : parent->children()) {
    if (!tree.empty()) {
      tree += parent->type() == Type::kSplit ? "|" : ",";
    }
    tree += Node(*child);
  }
  return tree;
}

GURL Url(const char* name) {
  return GURL(std::string("https://") + name + ".test/");
}

class LinkFoldersTest : public testing::Test {
 protected:
  SidebarNode* Tab(SpaceModel& model, SidebarNode* parent, const char* title) {
    return model.AddTab(parent, parent->children().size(), Url(title),
                        base::UTF8ToUTF16(title));
  }
  SidebarNode* Link(const SidebarNode& opener, const char* title) {
    return links_.AddLink(space_, opener, Url(title), base::UTF8ToUTF16(title));
  }

  SpaceModel space_;
  SpaceModel favorites_{SpaceModel::Kind::kFavorites};
  LinkFolders links_;
};

// An opener loose in Today gets a folder in its place, named after it; its
// links follow it there, each after the one before.
TEST_F(LinkFoldersTest, LinksGatherWithTheirOpenerInAFolder) {
  SidebarNode* today = space_.today_root();
  Tab(space_, today, "top");
  SidebarNode* opener = Tab(space_, today, "page");
  Tab(space_, today, "bottom");

  SidebarNode* a = Link(*opener, "a");
  ASSERT_TRUE(a);
  EXPECT_EQ(Tree(today), "top,page{page,a},bottom");
  SidebarNode* folder = opener->parent();
  EXPECT_EQ(folder->name_source(), NameSource::kOpener);
  EXPECT_TRUE(folder->link_folder_here());

  SidebarNode* b = Link(*opener, "b");
  EXPECT_EQ(Tree(today), "top,page{page,a,b},bottom");

  // A link from a link goes right after it, in the same folder.
  Link(*a, "c");
  EXPECT_EQ(Tree(today), "top,page{page,a,c,b},bottom");

  // The opener's previous link is gone: right after the opener.
  space_.Remove(b);
  Link(*opener, "d");
  EXPECT_EQ(Tree(today), "top,page{page,d,a,c},bottom");
}

// An opener in a folder of its own (made by hand) adds its links there.
TEST_F(LinkFoldersTest, OpenerInAFolderAddsToIt) {
  SidebarNode* today = space_.today_root();
  SidebarNode* folder = space_.AddFolder(today, 0, u"Reading");
  SidebarNode* opener = Tab(space_, folder, "page");
  Tab(space_, folder, "other");

  Link(*opener, "a");
  EXPECT_EQ(Tree(today), "Reading{page,a,other}");
  EXPECT_EQ(folder->name_source(), NameSource::kUser);
  EXPECT_FALSE(folder->link_folder_here());
}

// A split's pane opens next to its split, which goes in the folder whole,
// named after its first pane.
TEST_F(LinkFoldersTest, SplitOpenerGoesInWhole) {
  SidebarNode* today = space_.today_root();
  SidebarNode* left = Tab(space_, today, "left");
  SidebarNode* right = Tab(space_, today, "right");
  space_.Split(left, right);

  Link(*right, "a");
  EXPECT_EQ(Tree(today), "left{[left|right],a}");
}

// Pinned entries and favorites stay put; their links gather in a folder at
// the top of Today, which later links reuse while it's there.
TEST_F(LinkFoldersTest, LinksFromPinnedEntriesGatherAtTheTopOfToday) {
  SidebarNode* today = space_.today_root();
  Tab(space_, today, "loose");
  SidebarNode* pinned = Tab(space_, space_.pinned_root(), "docs");
  SidebarNode* favorite = Tab(favorites_, favorites_.pinned_root(), "mail");

  Link(*pinned, "a");
  Link(*pinned, "b");
  EXPECT_EQ(Tree(space_.pinned_root()), "docs");
  EXPECT_EQ(Tree(today), "docs{a,b},loose");
  EXPECT_TRUE(today->children()[0]->link_folder_here());

  Link(*favorite, "c");
  EXPECT_EQ(Tree(today), "mail{c},docs{a,b},loose");
  EXPECT_EQ(Tree(favorites_.pinned_root()), "mail");

  // Gone from Today: the next link makes a new one.
  space_.Remove(today->children()[1].get());
  Link(*pinned, "d");
  EXPECT_EQ(Tree(today), "docs{d},mail{c},loose");
}

// A folder is named after the opener's own name, else its title, else its
// host.
TEST_F(LinkFoldersTest, Names) {
  SidebarNode* today = space_.today_root();
  SidebarNode* named = Tab(space_, today, "page");
  space_.SetCustomTitle(named, u"My Page");
  SidebarNode* untitled =
      space_.AddTab(today, today->children().size(), Url("host"), u"");
  EXPECT_EQ(GetLinkFolderTitle(*named), u"My Page");
  EXPECT_EQ(GetLinkFolderTitle(*untitled), u"host.test");
}

// Only tabs open links.
TEST_F(LinkFoldersTest, NotFromAFolder) {
  SidebarNode* folder = space_.AddFolder(space_.today_root(), 0, u"F");
  EXPECT_FALSE(Link(*folder, "a"));
  EXPECT_EQ(Tree(space_.today_root()), "F{}");
}

}  // namespace
}  // namespace compass
