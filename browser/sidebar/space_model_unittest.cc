// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/sidebar/space_model.h"

#include <string>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
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
  void OnNodeWillMoveToSpace(const SidebarNode& node) override {
    events.push_back("leaving " + Name(node));
  }
  void OnSpaceChanged(const SpaceModel& space) override {
    events.push_back("space");
  }
  void OnArchiveChanged(const SpaceModel& space) override {
    events.push_back("archive");
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

TEST_F(SpaceModelTest, MovesEntriesToAnotherSpaceWithTheirIds) {
  SpaceModel other;
  SidebarNode* work = model_.AddFolder(model_.pinned_root(), 0, u"Work");
  SidebarNode* spec = Tab(work, "spec");
  const base::Uuid spec_id = spec->id();
  RecordingObserver source;
  RecordingObserver target;
  model_.AddObserver(&source);
  other.AddObserver(&target);

  model_.MoveToSpace(work, other, other.pinned_root(), 0);

  EXPECT_TRUE(model_.pinned_root()->children().empty());
  EXPECT_EQ(other.FindById(spec_id), spec);
  EXPECT_THAT(Titles(other.pinned_root()), ElementsAre("Work"));
  EXPECT_THAT(source.events, ElementsAre("leaving Work"));
  EXPECT_THAT(target.events, ElementsAre("added Work"));
  model_.RemoveObserver(&source);
  other.RemoveObserver(&target);
}

TEST_F(SpaceModelTest, MovingToAnotherSpaceKeepsTheInvariants) {
  SpaceModel other;
  SidebarNode* folder = model_.AddFolder(model_.pinned_root(), 0, u"F");
  SidebarNode* a = Tab(model_.today_root(), "a");
  SidebarNode* b = Tab(model_.today_root(), "b");
  model_.Split(a, b);

  // Folders stay in Pinned, nodes go to another space, roots stay put.
  EXPECT_FALSE(model_.CanMoveToSpace(folder, other, other.today_root()));
  EXPECT_FALSE(model_.CanMoveToSpace(a, model_, model_.pinned_root()));
  EXPECT_FALSE(
      model_.CanMoveToSpace(model_.today_root(), other, other.today_root()));
  EXPECT_FALSE(model_.CanMoveToSpace(a, other, model_.today_root()));

  // A pane leaving dissolves its split.
  model_.MoveToSpace(b, other, other.today_root(), 0);
  EXPECT_THAT(Titles(model_.today_root()), ElementsAre("a"));
  EXPECT_THAT(Titles(other.today_root()), ElementsAre("b"));
}

TEST_F(SpaceModelTest, NotifiesNameAndColorChanges) {
  RecordingObserver observer;
  model_.AddObserver(&observer);

  model_.SetName(u"Work");
  model_.SetName(u"Work");
  model_.SetColor(SpaceColor::kBlue);
  model_.SetColor(SpaceColor::kBlue);

  EXPECT_EQ(model_.color(), SpaceColor::kBlue);
  EXPECT_THAT(observer.events, ElementsAre("space", "space"));
  model_.RemoveObserver(&observer);
}

TEST(SpaceModelArchiveTest, ArchivesEntriesLeftUnseen) {
  SpaceModel model;
  SidebarNode* today = model.today_root();
  const base::Time start = base::Time::Now();
  SidebarNode* old_tab = model.AddTab(today, 0, GURL("https://old.test/"),
                                      u"Old");
  SidebarNode* recent = model.AddTab(today, 1, GURL("https://new.test/"),
                                     u"New");
  SidebarNode* unknown = model.AddTab(today, 2, GURL("https://u.test/"), u"U");
  SidebarNode* kept = model.AddTab(today, 3, GURL("https://k.test/"), u"Kept");
  SidebarNode* pinned = model.AddTab(model.pinned_root(), 0,
                                     GURL("https://p.test/"), u"Pinned");
  model.SetLastActive(old_tab, start - base::Hours(13));
  model.SetLastActive(recent, start - base::Hours(1));
  model.SetLastActive(kept, start - base::Days(3));
  model.SetLastActive(pinned, start - base::Days(3));

  EXPECT_EQ(model.ArchiveIdleEntries(
                start, [&](const SidebarNode& entry) { return &entry == kept; }),
            1u);

  EXPECT_THAT(Titles(today), ElementsAre("New", "U", "Kept"));
  EXPECT_THAT(Titles(model.pinned_root()), ElementsAre("Pinned"));
  // A tab with no time gets one, and a full delay.
  EXPECT_EQ(unknown->last_active(), start);
  ASSERT_EQ(model.archive().size(), 1u);
  EXPECT_EQ(model.archive()[0].tabs[0].title, u"Old");
  EXPECT_EQ(model.archive()[0].tabs[0].url, GURL("https://old.test/"));
  EXPECT_EQ(model.archive()[0].archived_at, start);

  const auto keep_none = [](const SidebarNode&) { return false; };
  EXPECT_EQ(model.ArchiveIdleEntries(start + base::Hours(11), keep_none), 2u);
  EXPECT_THAT(Titles(today), ElementsAre("U"));
  // Most recent first.
  ASSERT_EQ(model.archive().size(), 3u);
  EXPECT_EQ(model.archive()[0].tabs[0].title, u"Kept");
  EXPECT_EQ(model.archive()[1].tabs[0].title, u"New");
}

TEST(SpaceModelArchiveTest, SplitsGoByTheirMostRecentPane) {
  SpaceModel model;
  SidebarNode* today = model.today_root();
  const base::Time start = base::Time::Now();
  SidebarNode* a = model.AddTab(today, 0, GURL("https://a.test/"), u"A");
  SidebarNode* b = model.AddTab(today, 1, GURL("https://b.test/"), u"B");
  model.Split(a, b);
  model.SetLastActive(a, start - base::Days(2));
  model.SetLastActive(b, start - base::Hours(2));
  const auto keep_none = [](const SidebarNode&) { return false; };

  EXPECT_EQ(model.ArchiveIdleEntries(start, keep_none), 0u);
  EXPECT_EQ(model.ArchiveIdleEntries(start + base::Hours(10), keep_none), 1u);

  ASSERT_EQ(model.archive().size(), 1u);
  ASSERT_EQ(model.archive()[0].tabs.size(), 2u);
  SidebarNode* split = model.Unarchive(model.archive()[0].id, 0);
  ASSERT_TRUE(split);
  EXPECT_EQ(split->type(), Type::kSplit);
  EXPECT_THAT(Titles(today), ElementsAre("[A|B]"));
  EXPECT_TRUE(model.archive().empty());
}

TEST(SpaceModelArchiveTest, NeverWhenTheDelayIsZero) {
  SpaceModel model;
  SidebarNode* tab =
      model.AddTab(model.today_root(), 0, GURL("https://a.test/"), u"A");
  model.SetLastActive(tab, base::Time::Now() - base::Days(400));
  model.SetArchiveAfter(base::TimeDelta());

  EXPECT_EQ(model.ArchiveIdleEntries(base::Time::Now(),
                                     [](const SidebarNode&) { return false; }),
            0u);
  EXPECT_THAT(Titles(model.today_root()), ElementsAre("A"));
}

TEST(SpaceModelArchiveTest, KeepsOnlyPagesWorthRestoring) {
  SpaceModel model;
  SidebarNode* today = model.today_root();
  const base::Time now = base::Time::Now();
  model.Archive(model.AddTab(today, 0, GURL("chrome://newtab/"), u"NTP"), now);
  EXPECT_TRUE(today->children().empty());
  EXPECT_TRUE(model.archive().empty());

  for (size_t i = 0; i < kMaxArchivedEntries + 5; ++i) {
    model.Archive(model.AddTab(today, 0, GURL("https://a.test/"),
                               base::NumberToString16(i)),
                  now);
  }
  ASSERT_EQ(model.archive().size(), kMaxArchivedEntries);
  EXPECT_EQ(model.archive()[0].tabs[0].title,
            base::NumberToString16(kMaxArchivedEntries + 4));
}

TEST(SpaceModelArchiveTest, RemovesAndClears) {
  SpaceModel model;
  RecordingObserver observer;
  model.AddObserver(&observer);
  SidebarNode* today = model.today_root();
  const base::Time now = base::Time::Now();
  model.Archive(model.AddTab(today, 0, GURL("https://a.test/"), u"A"), now);
  model.Archive(model.AddTab(today, 0, GURL("https://b.test/"), u"B"), now);
  ASSERT_EQ(model.archive().size(), 2u);

  model.RemoveArchived(model.archive()[1].id);
  ASSERT_EQ(model.archive().size(), 1u);
  EXPECT_EQ(model.archive()[0].tabs[0].title, u"B");
  EXPECT_FALSE(model.Unarchive(base::Uuid::GenerateRandomV4(), 0));

  observer.events.clear();
  model.ClearArchive();
  EXPECT_TRUE(model.archive().empty());
  EXPECT_THAT(observer.events, ElementsAre("archive"));
  model.RemoveObserver(&observer);
}

}  // namespace
}  // namespace strata
