// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cmath>

#include "compass/browser/sidebar/space_model.h"

#include <string>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

using ::testing::Contains;
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

// Spaces sync unless set not to; a change tells observers once.
TEST_F(SpaceModelTest, SetSynced) {
  EXPECT_TRUE(model_.synced());
  RecordingObserver observer;
  model_.AddObserver(&observer);
  model_.SetSynced(false);
  model_.SetSynced(false);
  EXPECT_FALSE(model_.synced());
  EXPECT_EQ(observer.events, std::vector<std::string>({"space"}));
  model_.SetSynced(true);
  EXPECT_TRUE(model_.synced());
  model_.RemoveObserver(&observer);
}

TEST_F(SpaceModelTest, FoldersNestInPinnedAndToday) {
  SidebarNode* work = model_.AddFolder(model_.pinned_root(), 0, u"Work");
  SidebarNode* docs = model_.AddFolder(work, 0, u"Docs");
  Tab(docs, "spec");
  SidebarNode* reading = model_.AddFolder(model_.today_root(), 0, u"Reading");

  EXPECT_THAT(Titles(model_.pinned_root()), ElementsAre("Work"));
  EXPECT_THAT(Titles(docs), ElementsAre("spec"));
  EXPECT_TRUE(model_.CanMove(work, model_.today_root()));
  EXPECT_TRUE(model_.CanMove(docs, reading));
  EXPECT_FALSE(model_.CanMove(work, docs));  // Into its own descendant.
  EXPECT_TRUE(model_.CanMove(docs, model_.pinned_root()));
}

TEST_F(SpaceModelTest, EmptiedTodayFoldersClose) {
  SidebarNode* today = model_.today_root();
  SidebarNode* trip = model_.AddFolder(today, 0, u"Trip");
  SidebarNode* hotels = model_.AddFolder(trip, 0, u"Hotels");
  SidebarNode* hotel = Tab(hotels, "hotel");
  SidebarNode* flight = Tab(trip, "flight");
  SidebarNode* later = model_.AddFolder(today, 1, u"Later");

  // Made empty, a folder stays.
  EXPECT_THAT(Titles(today), ElementsAre("Trip", "Later"));

  // Its last child gone, it closes, and its emptied parents after it.
  model_.Remove(flight);
  EXPECT_THAT(Titles(trip), ElementsAre("Hotels"));
  model_.Move(hotel, today, 0);
  EXPECT_THAT(Titles(today), ElementsAre("hotel", "Later"));

  // Moved into another folder, or pinned, the same.
  SidebarNode* a = Tab(later, "a");
  model_.Move(a, model_.pinned_root(), 0);
  EXPECT_THAT(Titles(today), ElementsAre("hotel"));

  // Pinned folders stay.
  SidebarNode* work = model_.AddFolder(model_.pinned_root(), 1, u"Work");
  model_.Remove(Tab(work, "spec"));
  EXPECT_THAT(Titles(model_.pinned_root()), ElementsAre("a", "Work"));

  // A split's last pane leaving takes its folder too.
  SidebarNode* reading = model_.AddFolder(today, 0, u"Reading");
  SidebarNode* left = Tab(reading, "left");
  model_.Split(left, Tab(reading, "right"));
  model_.Remove(left);
  EXPECT_THAT(Titles(reading), ElementsAre("right"));
  model_.Remove(reading->children()[0].get());
  EXPECT_THAT(Titles(today), ElementsAre("hotel"));

  // Not while sync applies another device's changes.
  {
    SpaceModel::KeepEmptyFolders keep;
    SidebarNode* inbox = model_.AddFolder(today, 0, u"Inbox");
    model_.Remove(Tab(inbox, "b"));
    EXPECT_THAT(Titles(today), ElementsAre("Inbox", "hotel"));
  }
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
  static_assert(kMaxSplitPanes == 4);
  SidebarNode* a = Tab(model_.today_root(), "a");
  SidebarNode* b = Tab(model_.today_root(), "b");
  SidebarNode* c = Tab(model_.today_root(), "c");
  SidebarNode* d = Tab(model_.today_root(), "d");
  SidebarNode* e = Tab(model_.today_root(), "e");
  SidebarNode* split = model_.Split(a, b);

  // A tab dropped on a pane joins its split, at the end.
  EXPECT_TRUE(model_.CanSplit(a, c));
  EXPECT_EQ(model_.Split(a, c), split);
  EXPECT_TRUE(model_.CanMove(d, split));
  model_.Move(d, split, 1);
  EXPECT_THAT(Titles(model_.today_root()), ElementsAre("[a|d|b|c]", "e"));

  EXPECT_FALSE(model_.CanSplit(split, e));
  EXPECT_FALSE(model_.CanSplit(a, e));  // A pane stands for its split.
  EXPECT_FALSE(model_.CanMove(e, split));
  EXPECT_FALSE(model_.CanSplit(split, a));  // Already a pane.
  EXPECT_FALSE(model_.CanSplit(a, a));
}

TEST_F(SpaceModelTest, UnsplittingFourPutsEachInItsPlace) {
  Tab(model_.today_root(), "x");
  SidebarNode* a = Tab(model_.today_root(), "a");
  SidebarNode* b = Tab(model_.today_root(), "b");
  SidebarNode* c = Tab(model_.today_root(), "c");
  SidebarNode* d = Tab(model_.today_root(), "d");
  Tab(model_.today_root(), "y");
  SidebarNode* split = model_.Split(model_.Split(model_.Split(a, b), c), d);
  ASSERT_THAT(Titles(model_.today_root()),
              ElementsAre("x", "[a|b|c|d]", "y"));

  model_.Unsplit(split);

  EXPECT_THAT(Titles(model_.today_root()),
              ElementsAre("x", "a", "b", "c", "d", "y"));
}

TEST_F(SpaceModelTest, RemovingAPaneOfThreeKeepsTheSplit) {
  SidebarNode* a = Tab(model_.today_root(), "a");
  SidebarNode* b = Tab(model_.today_root(), "b");
  SidebarNode* c = Tab(model_.today_root(), "c");
  model_.Split(model_.Split(a, b), c);

  model_.Remove(b);

  EXPECT_THAT(Titles(model_.today_root()), ElementsAre("[a|c]"));
}

TEST_F(SpaceModelTest, UnsplitPutsThePanesInItsPlace) {
  Tab(model_.today_root(), "x");
  SidebarNode* a = Tab(model_.today_root(), "a");
  SidebarNode* b = Tab(model_.today_root(), "b");
  Tab(model_.today_root(), "y");
  SidebarNode* split = model_.Split(a, b);
  ASSERT_THAT(Titles(model_.today_root()), ElementsAre("x", "[a|b]", "y"));

  model_.Unsplit(split);

  EXPECT_THAT(Titles(model_.today_root()), ElementsAre("x", "a", "b", "y"));
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

TEST_F(SpaceModelTest, FoldersCollapse) {
  RecordingObserver observer;
  model_.AddObserver(&observer);
  SidebarNode* folder = model_.AddFolder(model_.pinned_root(), 0, u"Work");
  EXPECT_FALSE(folder->collapsed());

  model_.SetCollapsed(folder, true);
  model_.SetCollapsed(folder, true);
  EXPECT_TRUE(folder->collapsed());
  model_.SetCollapsed(folder, false);
  EXPECT_FALSE(folder->collapsed());

  // Unchanged state isn't announced.
  EXPECT_THAT(observer.events,
              ElementsAre("added Work", "changed Work", "changed Work"));
  model_.RemoveObserver(&observer);
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

  // Nodes go to another space, roots stay put.
  EXPECT_TRUE(model_.CanMoveToSpace(folder, other, other.today_root()));
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

TEST_F(SpaceModelTest, NotifiesIconChanges) {
  RecordingObserver observer;
  model_.AddObserver(&observer);

  model_.SetIcon(*SpaceIcon::Symbol("home"));
  model_.SetIcon(*SpaceIcon::Symbol("home"));
  model_.SetIcon(*SpaceIcon::Emoji("🚀"));
  model_.SetIcon(SpaceIcon());

  EXPECT_TRUE(model_.icon().is_none());
  EXPECT_THAT(observer.events, ElementsAre("space", "space", "space"));
  model_.RemoveObserver(&observer);
}

TEST_F(SpaceModelTest, GradientsNeedAColor) {
  RecordingObserver observer;
  model_.AddObserver(&observer);

  // Grey has none.
  model_.SetGradient({SpaceColor::kPink});
  EXPECT_TRUE(model_.gradient().empty());
  EXPECT_TRUE(observer.events.empty());

  model_.SetColor(SpaceColor::kBlue);
  // Grey entries and those past the limit are dropped.
  model_.SetGradient({SpaceColor::kPink, SpaceColor::kGrey, SpaceColor::kRed,
                      SpaceColor::kGreen});
  EXPECT_EQ(model_.gradient(),
            (std::vector<SpaceColor>{SpaceColor::kPink, SpaceColor::kRed}));
  model_.SetGradient({SpaceColor::kPink, SpaceColor::kRed});

  // Turning grey drops it.
  model_.SetColor(SpaceColor::kGrey);
  EXPECT_TRUE(model_.gradient().empty());
  EXPECT_THAT(observer.events, ElementsAre("space", "space", "space"));
  model_.RemoveObserver(&observer);
}

TEST_F(SpaceModelTest, IntensityAndGrainStayInRange) {
  EXPECT_EQ(model_.intensity(), kDefaultSpaceIntensity);
  EXPECT_EQ(model_.grain(), 0);
  RecordingObserver observer;
  model_.AddObserver(&observer);

  model_.SetIntensity(0.8);
  model_.SetIntensity(0.8);
  model_.SetGrain(2);
  model_.SetGrain(std::nan(""));
  model_.SetIntensity(-1);

  EXPECT_EQ(model_.intensity(), 0);
  EXPECT_EQ(model_.grain(), 1);
  EXPECT_THAT(observer.events, ElementsAre("space", "space", "space"));
  model_.RemoveObserver(&observer);
}

TEST(SpaceIconTest, AcceptsEmojiAndSymbolNamesOnly) {
  EXPECT_EQ(SpaceIcon::Emoji("🚑")->value(), "🚑");
  // A whole sequence: family, flag.
  EXPECT_TRUE(SpaceIcon::Emoji("👨‍👩‍👧‍👦"));
  EXPECT_TRUE(SpaceIcon::Emoji("🇮🇹"));
  EXPECT_FALSE(SpaceIcon::Emoji(""));
  EXPECT_FALSE(SpaceIcon::Emoji(" "));
  EXPECT_FALSE(SpaceIcon::Emoji("a\nb"));
  EXPECT_FALSE(SpaceIcon::Emoji("\xff"));
  EXPECT_FALSE(SpaceIcon::Emoji(std::string(65, 'a')));

  EXPECT_EQ(SpaceIcon::Symbol("travel-2")->kind(), SpaceIcon::Kind::kSymbol);
  EXPECT_FALSE(SpaceIcon::Symbol(""));
  EXPECT_FALSE(SpaceIcon::Symbol("Home"));
  EXPECT_FALSE(SpaceIcon::Symbol("../x"));
  EXPECT_FALSE(SpaceIcon::Symbol(std::string(33, 'a')));
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

TEST(SpaceModelArchiveTest, ArchivesEntriesInFoldersOneByOne) {
  SpaceModel model;
  SidebarNode* today = model.today_root();
  const base::Time start = base::Time::Now();
  SidebarNode* folder = model.AddFolder(today, 0, u"Trip");
  SidebarNode* old_tab =
      model.AddTab(folder, 0, GURL("https://old.test/"), u"Old");
  SidebarNode* recent =
      model.AddTab(folder, 1, GURL("https://new.test/"), u"New");
  model.SetLastActive(old_tab, start - base::Days(2));
  model.SetLastActive(recent, start - base::Hours(1));
  const auto keep_none = [](const SidebarNode&) { return false; };

  EXPECT_EQ(model.ArchiveIdleEntries(start, keep_none), 1u);
  EXPECT_THAT(Titles(today), ElementsAre("Trip"));
  EXPECT_THAT(Titles(folder), ElementsAre("New"));

  // Emptied, the folder closes.
  EXPECT_EQ(model.ArchiveIdleEntries(start + base::Days(1), keep_none), 1u);
  EXPECT_TRUE(today->children().empty());
  EXPECT_EQ(model.archive().size(), 2u);
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

TEST(SpaceModelArchiveTest, EntriesKeepTheirNodesIds) {
  SpaceModel model;
  SidebarNode* today = model.today_root();
  const base::Time now = base::Time::Now();
  SidebarNode* tab = model.AddTab(today, 0, GURL("https://t.test/"), u"T");
  const base::Uuid tab_id = tab->id();
  model.Archive(tab, now);
  ASSERT_EQ(model.archive().size(), 1u);
  EXPECT_EQ(model.archive()[0].id, tab_id);
  EXPECT_EQ(model.archive()[0].tabs[0].id, tab_id);

  // A split with one page worth keeping is archived as that page.
  SidebarNode* a = model.AddTab(today, 0, GURL("https://a.test/"), u"A");
  SidebarNode* blank = model.AddTab(today, 1, GURL("about:blank"), u"");
  const base::Uuid a_id = a->id();
  model.Archive(model.Split(a, blank), now + base::Seconds(1));
  ASSERT_EQ(model.archive().size(), 2u);
  EXPECT_EQ(model.archive()[0].id, a_id);

  // Restoring makes new nodes.
  SidebarNode* restored = model.Unarchive(tab_id, 0);
  ASSERT_TRUE(restored);
  EXPECT_NE(restored->id(), tab_id);
}

TEST(SpaceModelArchiveTest, PutArchivedGoesByTimeAndReplaces) {
  SpaceModel model;
  RecordingObserver observer;
  model.AddObserver(&observer);
  const base::Time now = base::Time::Now();
  const auto entry = [](const base::Uuid& id, base::Time at,
                        std::u16string title) {
    ArchivedEntry archived;
    archived.id = id;
    archived.archived_at = at;
    archived.tabs.push_back({std::move(title), GURL("https://x.test/"), id});
    return archived;
  };
  const base::Uuid old_id = base::Uuid::GenerateRandomV4();
  const base::Uuid new_id = base::Uuid::GenerateRandomV4();
  model.PutArchived(entry(old_id, now - base::Hours(2), u"Old"));
  model.PutArchived(entry(new_id, now, u"New"));
  model.PutArchived(entry(base::Uuid::GenerateRandomV4(),
                          now - base::Hours(1), u"Middle"));
  ASSERT_EQ(model.archive().size(), 3u);
  EXPECT_EQ(model.archive()[0].tabs[0].title, u"New");
  EXPECT_EQ(model.archive()[1].tabs[0].title, u"Middle");
  EXPECT_EQ(model.archive()[2].tabs[0].title, u"Old");
  EXPECT_THAT(observer.events, Contains("archive"));

  // The same ID replaces, in its new place.
  model.PutArchived(entry(old_id, now + base::Hours(1), u"Again"));
  ASSERT_EQ(model.archive().size(), 3u);
  EXPECT_EQ(model.archive()[0].tabs[0].title, u"Again");
  EXPECT_EQ(model.FindArchived(old_id)->tabs[0].title, u"Again");
  model.RemoveObserver(&observer);
}

TEST(SpaceModelFavoritesTest, HoldOnlyTabs) {
  SpaceModel favorites(SpaceModel::Kind::kFavorites);
  SidebarNode* root = favorites.pinned_root();
  SidebarNode* a = favorites.AddTab(root, 0, GURL("https://a.test/"), u"A");
  SidebarNode* b = favorites.AddTab(root, 1, GURL("https://b.test/"), u"B");

  EXPECT_FALSE(favorites.CanSplit(a, b));
  EXPECT_FALSE(favorites.CanMove(a, favorites.today_root()));

  SpaceModel space;
  SidebarNode* folder = space.AddFolder(space.pinned_root(), 0, u"F");
  SidebarNode* c = space.AddTab(space.today_root(), 0, GURL("https://c.test/"),
                                u"C");
  SidebarNode* d = space.AddTab(space.today_root(), 1, GURL("https://d.test/"),
                                u"D");
  SidebarNode* split = space.Split(c, d);
  EXPECT_FALSE(space.CanMoveToSpace(folder, favorites, root));
  EXPECT_FALSE(space.CanMoveToSpace(split, favorites, root));
  EXPECT_FALSE(space.CanMoveToSpace(d, favorites, favorites.today_root()));

  // A pane can become a favorite; its split dissolves.
  space.MoveToSpace(d, favorites, root, 0);
  EXPECT_THAT(Titles(root), ElementsAre("D", "A", "B"));
  EXPECT_THAT(Titles(space.today_root()), ElementsAre("C"));
  // And back.
  EXPECT_TRUE(favorites.CanMoveToSpace(d, space, folder));
  favorites.MoveToSpace(d, space, folder, 0);
  EXPECT_THAT(Titles(folder), ElementsAre("D"));
}

// docs/design/sidebar.md, "Pinned URL".
TEST_F(SpaceModelTest, PinnedTabsKeepASavedUrl) {
  SidebarNode* a = Tab(model_.today_root(), "a");
  EXPECT_TRUE(a->saved_url().is_empty());

  // Pinning saves the page it shows; navigating doesn't move it.
  model_.Move(a, model_.pinned_root(), 0);
  EXPECT_EQ(a->saved_url(), GURL("https://a.test/"));
  EXPECT_EQ(a->saved_title(), u"a");
  model_.SetUrl(a, GURL("https://a.test/elsewhere"));
  EXPECT_EQ(a->saved_url(), GURL("https://a.test/"));

  // Moving it within Pinned keeps it; setting it tells observers.
  SidebarNode* folder = model_.AddFolder(model_.pinned_root(), 1, u"F");
  model_.Move(a, folder, 0);
  EXPECT_EQ(a->saved_url(), GURL("https://a.test/"));
  RecordingObserver observer;
  model_.AddObserver(&observer);
  model_.SetSavedUrl(a, a->url(), u"Elsewhere");
  model_.RemoveObserver(&observer);
  EXPECT_EQ(a->saved_url(), GURL("https://a.test/elsewhere"));
  EXPECT_EQ(a->saved_title(), u"Elsewhere");
  EXPECT_FALSE(observer.events.empty());

  // Unpinning drops it, with the folder too.
  model_.Move(folder, model_.today_root(), 0);
  EXPECT_TRUE(a->saved_url().is_empty());
  EXPECT_TRUE(a->saved_title().empty());

  // Tabs added to Pinned get one, panes of a pinned split too.
  SidebarNode* b = Tab(model_.pinned_root(), "b");
  SidebarNode* c = Tab(model_.pinned_root(), "c");
  model_.Split(b, c);
  EXPECT_EQ(b->saved_url(), GURL("https://b.test/"));
  EXPECT_EQ(c->saved_url(), GURL("https://c.test/"));

  // Favorites are all pinned.
  SpaceModel favorites(SpaceModel::Kind::kFavorites);
  SidebarNode* d = favorites.AddTab(favorites.pinned_root(), 0,
                                    GURL("https://d.test/"), u"D");
  EXPECT_EQ(d->saved_url(), GURL("https://d.test/"));
}


// Live folders (docs/design/live-folders.md).

LiveSource RssSource() {
  return LiveSource("rss", base::DictValue().Set("url", "https://feed.test/"));
}

std::vector<LiveItem> Items(std::initializer_list<const char*> keys) {
  std::vector<LiveItem> items;
  for (const char* key : keys) {
    items.push_back({key, GURL(std::string("https://") + key + ".test/"),
                     base::UTF8ToUTF16(key)});
  }
  return items;
}

constexpr auto NoneOpen = [](const SidebarNode&) { return false; };

TEST_F(SpaceModelTest, LiveFoldersOnlyInPinned) {
  SidebarNode* work = model_.AddFolder(model_.pinned_root(), 0, u"Work");
  SidebarNode* today_folder =
      model_.AddFolder(model_.today_root(), 0, u"Reading");
  EXPECT_TRUE(model_.CanAddLiveFolder(model_.pinned_root()));
  EXPECT_TRUE(model_.CanAddLiveFolder(work));
  EXPECT_FALSE(model_.CanAddLiveFolder(model_.today_root()));
  EXPECT_FALSE(model_.CanAddLiveFolder(today_folder));
  SpaceModel favorites(SpaceModel::Kind::kFavorites);
  EXPECT_FALSE(favorites.CanAddLiveFolder(favorites.pinned_root()));

  SidebarNode* live =
      model_.AddLiveFolder(work, 0, u"News", RssSource());
  EXPECT_TRUE(live->is_live_folder());
  EXPECT_EQ(live->live_source()->plugin_id, "rss");
  EXPECT_FALSE(model_.CanAddLiveFolder(live));

  // It moves around Pinned, alone or in its folder, but not to Today.
  EXPECT_TRUE(model_.CanMove(live, model_.pinned_root()));
  EXPECT_FALSE(model_.CanMove(live, model_.today_root()));
  EXPECT_FALSE(model_.CanMove(live, today_folder));
  EXPECT_FALSE(model_.CanMove(work, model_.today_root()));

  SpaceModel other;
  EXPECT_TRUE(model_.CanMoveToSpace(live, other, other.pinned_root()));
  EXPECT_FALSE(model_.CanMoveToSpace(live, other, other.today_root()));
  EXPECT_FALSE(model_.CanMoveToSpace(work, other, other.today_root()));
}

TEST_F(SpaceModelTest, LiveFoldersHoldOnlyTheirEntries) {
  SidebarNode* live =
      model_.AddLiveFolder(model_.pinned_root(), 0, u"News", RssSource());
  model_.ReplaceLiveItems(live, Items({"a", "b"}), base::Time::Now(),
                          NoneOpen);
  SidebarNode* a = live->children()[0].get();
  SidebarNode* b = live->children()[1].get();
  SidebarNode* tab = Tab(model_.pinned_root(), "tab");
  SidebarNode* folder = model_.AddFolder(model_.pinned_root(), 0, u"Folder");

  EXPECT_FALSE(model_.CanMove(tab, live));
  EXPECT_FALSE(model_.CanMove(folder, live));
  // Entries keep the plugin's order, and stay single tabs.
  EXPECT_FALSE(model_.CanMove(b, live));
  EXPECT_FALSE(model_.CanSplit(a, tab));
  // An entry can join a split elsewhere: it leaves the folder.
  EXPECT_TRUE(model_.CanSplit(tab, a));
  SpaceModel other;
  SidebarNode* elsewhere =
      other.AddLiveFolder(other.pinned_root(), 0, u"Other", RssSource());
  EXPECT_FALSE(model_.CanMoveToSpace(tab, other, elsewhere));
}

TEST_F(SpaceModelTest, EntriesMovedOutBecomeOrdinaryTabs) {
  SidebarNode* live =
      model_.AddLiveFolder(model_.pinned_root(), 0, u"News", RssSource());
  model_.ReplaceLiveItems(live, Items({"a"}), base::Time::Now(), NoneOpen);
  model_.ReplaceLiveItems(live, Items({"a", "b", "c"}), base::Time::Now(),
                          NoneOpen);
  SidebarNode* b = live->children()[1].get();
  SidebarNode* c = live->children()[2].get();
  ASSERT_TRUE(b->unseen());

  model_.Move(b, model_.pinned_root(), 0);
  EXPECT_FALSE(b->is_live_item());
  EXPECT_FALSE(b->unseen());
  EXPECT_EQ(b->saved_url(), GURL("https://b.test/"));

  // Into a split, or another space, too.
  SidebarNode* d = Tab(model_.pinned_root(), "d");
  model_.Split(d, c);
  EXPECT_FALSE(c->is_live_item());

  SidebarNode* a = live->children()[0].get();
  SpaceModel other;
  model_.MoveToSpace(a, other, other.today_root(), 0);
  EXPECT_FALSE(a->is_live_item());

  // A later refresh brings them back as new entries.
  model_.ReplaceLiveItems(live, Items({"a", "b"}), base::Time::Now(),
                          NoneOpen);
  EXPECT_THAT(Titles(live), ElementsAre("a", "b"));
  EXPECT_NE(live->children()[1].get(), b);
}

TEST_F(SpaceModelTest, ReplaceLiveItemsDiffsByKey) {
  SidebarNode* live =
      model_.AddLiveFolder(model_.pinned_root(), 0, u"News", RssSource());
  const base::Time first = base::Time::Now();
  model_.ReplaceLiveItems(live, Items({"a", "b", "c"}), first, NoneOpen);
  EXPECT_THAT(Titles(live), ElementsAre("a", "b", "c"));
  EXPECT_EQ(live->live_refreshed(), first);
  // The first fill isn't news.
  for (const auto& entry : live->children()) {
    EXPECT_TRUE(entry->is_live_item());
    EXPECT_FALSE(entry->unseen());
    EXPECT_EQ(entry->saved_url(), entry->url());
  }
  const base::Uuid a_id = live->children()[0]->id();
  const base::Uuid c_id = live->children()[2]->id();

  RecordingObserver observer;
  model_.AddObserver(&observer);
  std::vector<LiveItem> items = Items({"d", "c", "a", "d", ""});
  items[2].title = u"A2";
  items[2].url = GURL("https://a.test/2");
  model_.ReplaceLiveItems(live, items, first + base::Minutes(10), NoneOpen);
  model_.RemoveObserver(&observer);

  // Plugin order; the repeated and keyless items are skipped.
  EXPECT_THAT(Titles(live), ElementsAre("d", "c", "A2"));
  EXPECT_TRUE(live->children()[0]->unseen());
  EXPECT_EQ(live->children()[1]->id(), c_id);
  EXPECT_FALSE(live->children()[1]->unseen());
  EXPECT_EQ(live->children()[2]->id(), a_id);
  EXPECT_EQ(live->children()[2]->url(), GURL("https://a.test/2"));
  EXPECT_EQ(live->children()[2]->saved_url(), GURL("https://a.test/2"));
  EXPECT_EQ(live->live_refreshed(), first + base::Minutes(10));
  EXPECT_THAT(observer.events, Contains("added d"));
  EXPECT_THAT(observer.events, Contains("changed A2"));
  EXPECT_THAT(observer.events, Contains("removed b"));
  EXPECT_EQ(observer.events.back(), "changed News");

  model_.MarkAllSeen(live);
  EXPECT_FALSE(live->children()[0]->unseen());
}

TEST_F(SpaceModelTest, ReplaceLiveItemsKeepsOpenTabs) {
  SidebarNode* live =
      model_.AddLiveFolder(model_.pinned_root(), 0, u"News", RssSource());
  model_.ReplaceLiveItems(live, Items({"a", "b"}), base::Time::Now(),
                          NoneOpen);
  SidebarNode* a = live->children()[0].get();
  SidebarNode* b = live->children()[1].get();
  // a's tab went elsewhere.
  model_.SetUrl(a, GURL("https://a.test/next"));
  auto open = [&](const SidebarNode& node) { return &node == a || &node == b; };

  std::vector<LiveItem> items = Items({"a", "c"});
  items[0].url = GURL("https://a.test/new");
  model_.ReplaceLiveItems(live, items, base::Time::Now(), open);

  // b vanished but is open: it stays, after the others. a keeps its page.
  EXPECT_THAT(Titles(live), ElementsAre("a", "c", "b"));
  EXPECT_EQ(a->url(), GURL("https://a.test/next"));
  EXPECT_EQ(a->saved_url(), GURL("https://a.test/new"));
  EXPECT_TRUE(b->is_live_item());

  // Once closed, the next refresh drops it.
  model_.ReplaceLiveItems(live, items, base::Time::Now(), NoneOpen);
  EXPECT_THAT(Titles(live), ElementsAre("a", "c"));
}

TEST_F(SpaceModelTest, ConvertToPlainFolderKeepsEntries) {
  SidebarNode* live =
      model_.AddLiveFolder(model_.pinned_root(), 0, u"News", RssSource());
  model_.ReplaceLiveItems(live, Items({"a"}), base::Time::Now(), NoneOpen);
  model_.ReplaceLiveItems(live, Items({"a", "b"}), base::Time::Now(),
                          NoneOpen);

  model_.ConvertToPlainFolder(live);

  EXPECT_FALSE(live->is_live_folder());
  EXPECT_TRUE(live->live_refreshed().is_null());
  EXPECT_THAT(Titles(live), ElementsAre("a", "b"));
  for (const auto& entry : live->children()) {
    EXPECT_FALSE(entry->is_live_item());
    EXPECT_FALSE(entry->unseen());
  }
  // Now anything goes, Today included.
  EXPECT_TRUE(model_.CanMove(Tab(model_.pinned_root(), "x"), live));
  EXPECT_TRUE(model_.CanMove(live, model_.today_root()));
}

TEST_F(SpaceModelTest, SetLiveSource) {
  SidebarNode* live =
      model_.AddLiveFolder(model_.pinned_root(), 0, u"News", RssSource());
  RecordingObserver observer;
  model_.AddObserver(&observer);
  model_.SetLiveSource(live, RssSource());
  EXPECT_TRUE(observer.events.empty());
  model_.SetLiveSource(
      live, LiveSource("github", base::DictValue().Set("query", "is:pr")));
  model_.RemoveObserver(&observer);
  EXPECT_EQ(live->live_source()->plugin_id, "github");
  EXPECT_EQ(*live->live_source()->config.FindString("query"), "is:pr");
  EXPECT_THAT(observer.events, ElementsAre("changed News"));
}

}  // namespace
}  // namespace compass
