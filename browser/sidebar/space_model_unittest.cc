// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/sidebar/space_model.h"

#include <string>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace strata {
namespace {

using ::testing::ElementsAre;
using Type = SidebarNode::Type;

// Titles of |parent|'s children, with splits shown as "[pane|pane]".
std::vector<std::string> Titles(const SidebarNode* parent) {
  std::vector<std::string> titles;
  for (const auto& child : parent->children()) {
    if (child->type() == Type::kSplit) {
      std::string split;
      for (const auto& pane : child->children()) {
        split += (split.empty() ? "" : "|") + base::UTF16ToUTF8(pane->title());
      }
      titles.push_back("[" + split + "]");
    } else {
      titles.push_back(base::UTF16ToUTF8(child->title()));
    }
  }
  return titles;
}

class RecordingObserver : public SpaceModel::Observer {
 public:
  void OnNodeAdded(const SidebarNode& node) override {
    events.push_back("added " + Name(node));
  }
  void OnNodeMoved(const SidebarNode& node,
                   const SidebarNode& old_parent,
                   size_t old_index) override {
    events.push_back("moved " + Name(node));
  }
  void OnNodeChanged(const SidebarNode& node) override {
    events.push_back("changed " + Name(node));
  }
  void OnNodeWillBeRemoved(const SidebarNode& node) override {
    events.push_back("removed " + Name(node));
  }

  std::vector<std::string> events;

 private:
  static std::string Name(const SidebarNode& node) {
    return node.type() == Type::kSplit ? "split"
                                       : base::UTF16ToUTF8(node.title());
  }
};

class SpaceModelTest : public testing::Test {
 protected:
  SidebarNode* Tab(SidebarNode* parent, const char* title) {
    return model_.AddTab(parent, parent->children().size(),
                         GURL(std::string("https://") + title + ".test/"),
                         base::UTF8ToUTF16(title));
  }

  SpaceModel model_;
};

TEST_F(SpaceModelTest, StartsEmpty) {
  EXPECT_TRUE(model_.pinned_root()->children().empty());
  EXPECT_TRUE(model_.today_root()->children().empty());
  EXPECT_FALSE(model_.CanMove(model_.pinned_root(), model_.today_root()));
}

TEST_F(SpaceModelTest, FoldersNestInPinnedOnly) {
  SidebarNode* work = model_.AddFolder(model_.pinned_root(), 0, u"Work");
  SidebarNode* docs = model_.AddFolder(work, 0, u"Docs");
  Tab(docs, "spec");

  EXPECT_THAT(Titles(model_.pinned_root()), ElementsAre("Work"));
  EXPECT_THAT(Titles(docs), ElementsAre("spec"));
  EXPECT_FALSE(model_.CanMove(work, model_.today_root()));
  EXPECT_FALSE(model_.CanMove(work, docs));  // Into its own descendant.
  EXPECT_TRUE(model_.CanMove(docs, model_.pinned_root()));
}

TEST_F(SpaceModelTest, MoveIndexIsCountedAfterDetaching) {
  SidebarNode* a = Tab(model_.today_root(), "a");
  Tab(model_.today_root(), "b");
  Tab(model_.today_root(), "c");

  model_.Move(a, model_.today_root(), 2);

  EXPECT_THAT(Titles(model_.today_root()), ElementsAre("b", "c", "a"));
}

TEST_F(SpaceModelTest, SplitIsOneEntryInTargetsPosition) {
  SidebarNode* a = Tab(model_.today_root(), "a");
  Tab(model_.today_root(), "b");
  SidebarNode* c = Tab(model_.today_root(), "c");

  SidebarNode* split = model_.Split(a, c);

  EXPECT_EQ(split->type(), Type::kSplit);
  EXPECT_THAT(Titles(model_.today_root()), ElementsAre("[a|c]", "b"));
}

TEST_F(SpaceModelTest, SplitAcrossSectionsLandsInTargetsSection) {
  SidebarNode* pinned = Tab(model_.pinned_root(), "pinned");
  SidebarNode* today = Tab(model_.today_root(), "today");

  model_.Split(pinned, today);

  EXPECT_THAT(Titles(model_.pinned_root()), ElementsAre("[pinned|today]"));
  EXPECT_TRUE(model_.today_root()->children().empty());
}

TEST_F(SpaceModelTest, SplitRespectsPaneLimit) {
  SidebarNode* a = Tab(model_.today_root(), "a");
  SidebarNode* b = Tab(model_.today_root(), "b");
  SidebarNode* c = Tab(model_.today_root(), "c");
  SidebarNode* split = model_.Split(a, b);

  EXPECT_FALSE(model_.CanSplit(split, c));
  EXPECT_FALSE(model_.CanSplit(a, c));  // A pane stands for its split.
  EXPECT_FALSE(model_.CanMove(c, split));
  EXPECT_FALSE(model_.CanSplit(split, a));  // Already a pane.
  EXPECT_FALSE(model_.CanSplit(a, a));
}

TEST_F(SpaceModelTest, SplitsCannotNestOrHoldFolders) {
  SidebarNode* folder = model_.AddFolder(model_.pinned_root(), 0, u"f");
  SidebarNode* a = Tab(model_.pinned_root(), "a");
  SidebarNode* b = Tab(model_.pinned_root(), "b");
  SidebarNode* split = model_.Split(a, b);

  EXPECT_FALSE(model_.CanMove(folder, split));
  EXPECT_FALSE(model_.CanSplit(folder, a));
}

TEST_F(SpaceModelTest, RemovingAPaneDissolvesTheSplit) {
  SidebarNode* a = Tab(model_.today_root(), "a");
  SidebarNode* b = Tab(model_.today_root(), "b");
  Tab(model_.today_root(), "c");
  model_.Split(a, b);

  model_.Remove(a);

  EXPECT_THAT(Titles(model_.today_root()), ElementsAre("b", "c"));
  EXPECT_EQ(b->parent(), model_.today_root());
}

TEST_F(SpaceModelTest, PullingAPaneOutDissolvesTheSplit) {
  Tab(model_.today_root(), "x");
  SidebarNode* a = Tab(model_.today_root(), "a");
  SidebarNode* b = Tab(model_.today_root(), "b");
  model_.Split(a, b);

  model_.Move(a, model_.pinned_root(), 0);

  EXPECT_THAT(Titles(model_.pinned_root()), ElementsAre("a"));
  EXPECT_THAT(Titles(model_.today_root()), ElementsAre("x", "b"));
}

TEST_F(SpaceModelTest, ReorderingPanesKeepsTheSplit) {
  SidebarNode* a = Tab(model_.today_root(), "a");
  SidebarNode* b = Tab(model_.today_root(), "b");
  SidebarNode* split = model_.Split(a, b);

  model_.Move(b, split, 0);

  EXPECT_THAT(Titles(model_.today_root()), ElementsAre("[b|a]"));
}

TEST_F(SpaceModelTest, RemovingAFolderRemovesItsSubtree) {
  SidebarNode* folder = model_.AddFolder(model_.pinned_root(), 0, u"f");
  SidebarNode* tab = Tab(folder, "a");
  const base::Uuid tab_id = tab->id();

  model_.Remove(folder);

  EXPECT_TRUE(model_.pinned_root()->children().empty());
  EXPECT_EQ(model_.FindById(tab_id), nullptr);
}

TEST_F(SpaceModelTest, FindsNodesById) {
  SidebarNode* folder = model_.AddFolder(model_.pinned_root(), 0, u"f");
  SidebarNode* tab = Tab(folder, "a");

  EXPECT_EQ(model_.FindById(tab->id()), tab);
  EXPECT_EQ(model_.FindById(base::Uuid::GenerateRandomV4()), nullptr);
}

TEST_F(SpaceModelTest, NotifiesObservers) {
  RecordingObserver observer;
  model_.AddObserver(&observer);

  SidebarNode* a = Tab(model_.today_root(), "a");
  SidebarNode* b = Tab(model_.today_root(), "b");
  model_.SetTitle(a, u"A");
  model_.Split(a, b);
  model_.Remove(b);

  EXPECT_THAT(observer.events,
              ElementsAre("added a", "added b", "changed A", "added split",
                          "moved A", "moved b", "removed b", "moved A",
                          "removed split"));
  model_.RemoveObserver(&observer);
}

}  // namespace
}  // namespace strata
