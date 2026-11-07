// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/closed_entries.h"

#include <optional>
#include <string>
#include <vector>

#include "base/strings/string_number_conversions.h"
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

// Titles of |parent|'s children, with splits as "[pane|pane]" and folders as
// "{child,child}".
std::vector<std::string> Titles(const SidebarNode* parent) {
  std::vector<std::string> titles;
  for (const auto& child : parent->children()) {
    std::string title;
    if (child->type() == Type::kTab) {
      title = base::UTF16ToUTF8(child->title());
    } else {
      for (const std::string& inner : Titles(child.get())) {
        title += (title.empty() ? "" : child->type() == Type::kSplit ? "|"
                                                                     : ",") +
                 inner;
      }
      title = child->type() == Type::kSplit ? "[" + title + "]"
                                            : "{" + title + "}";
    }
    titles.push_back(title);
  }
  return titles;
}

class ClosedEntriesTest : public testing::Test {
 protected:
  SidebarNode* AddTab(SidebarNode* parent, const std::string& name) {
    return model_.AddTab(parent, parent->children().size(),
                         GURL("https://" + name + ".test/"),
                         base::UTF8ToUTF16(name));
  }

  SidebarNode* today() { return model_.today_root(); }
  SidebarNode* pinned() { return model_.pinned_root(); }

  // Undoes the latest step into the model; returns the tab to open.
  SidebarNode* Undo() {
    std::optional<ClosedEntries::Step> step = closed_.TakeUndo();
    if (!step) {
      return nullptr;
    }
    SidebarNode* reopen = ClosedEntries::Restore(
        *step, model_, favorites_, [](const SidebarNode&) { return false; });
    closed_.PushRedo(std::move(*step));
    return reopen;
  }

  SpaceModel model_;
  SpaceModel favorites_{SpaceModel::Kind::kFavorites};
  ClosedEntries closed_;
};

TEST_F(ClosedEntriesTest, ATabComesBackWhereItWasWithItsId) {
  AddTab(today(), "a");
  SidebarNode* b = AddTab(today(), "b");
  AddTab(today(), "c");
  const base::Uuid id = b->id();
  model_.SetCustomTitle(b, u"Bee");

  closed_.RecordRemoval(model_, *b);
  model_.Remove(b);
  ASSERT_THAT(Titles(today()), ElementsAre("a", "c"));

  Undo();
  EXPECT_THAT(Titles(today()), ElementsAre("a", "b", "c"));
  SidebarNode* back = model_.FindById(id);
  ASSERT_TRUE(back);
  EXPECT_EQ(back->custom_title(), u"Bee");
  EXPECT_FALSE(closed_.CanUndo());
  EXPECT_TRUE(closed_.CanRedo());
}

TEST_F(ClosedEntriesTest, AStepComesBackInOrderWithItsFolders) {
  AddTab(today(), "a");
  SidebarNode* folder = model_.AddFolder(today(), 1, u"f");
  AddTab(folder, "x");
  AddTab(folder, "y");
  AddTab(today(), "c");

  {
    // As Clear Today records: everything first, then the closes.
    ClosedEntries::ScopedStep step(closed_);
    for (const auto& entry : today()->children()) {
      closed_.RecordRemoval(model_, *entry);
    }
    while (!today()->children().empty()) {
      model_.Remove(today()->children().back().get());
    }
  }
  ASSERT_EQ(closed_.undo_steps_for_testing().size(), 1u);

  Undo();
  EXPECT_THAT(Titles(today()), ElementsAre("a", "{x,y}", "c"));
}

TEST_F(ClosedEntriesTest, AnEmptiedTodayFolderComesBackWithItsTab) {
  AddTab(today(), "a");
  SidebarNode* folder = model_.AddFolder(today(), 1, u"f");
  SidebarNode* x = AddTab(folder, "x");

  closed_.RecordRemoval(model_, *x);
  model_.Remove(x);
  ASSERT_THAT(Titles(today()), ElementsAre("a"));

  Undo();
  EXPECT_THAT(Titles(today()), ElementsAre("a", "{x}"));
}

TEST_F(ClosedEntriesTest, APaneComesBackIntoItsSplit) {
  SidebarNode* a = AddTab(today(), "a");
  SidebarNode* b = AddTab(today(), "b");
  model_.Split(a, b);
  ASSERT_THAT(Titles(today()), ElementsAre("[a|b]"));

  closed_.RecordRemoval(model_, *a);
  model_.Remove(a);
  ASSERT_THAT(Titles(today()), ElementsAre("b"));

  Undo();
  EXPECT_THAT(Titles(today()), ElementsAre("[a|b]"));
}

TEST_F(ClosedEntriesTest, AWholeSplitComesBack) {
  SidebarNode* a = AddTab(today(), "a");
  SidebarNode* b = AddTab(today(), "b");
  SidebarNode* split = model_.Split(a, b);

  closed_.RecordRemoval(model_, *split);
  model_.Remove(split);

  Undo();
  EXPECT_THAT(Titles(today()), ElementsAre("[a|b]"));
}

TEST_F(ClosedEntriesTest, AnEntryWhoseParentWentGoesToTheTopOfToday) {
  AddTab(today(), "a");
  SidebarNode* folder = model_.AddFolder(today(), 1, u"f");
  SidebarNode* x = AddTab(folder, "x");
  AddTab(folder, "y");

  closed_.RecordRemoval(model_, *x);
  model_.Remove(x);
  model_.Remove(folder);

  Undo();
  EXPECT_THAT(Titles(today()), ElementsAre("x", "a"));
}

TEST_F(ClosedEntriesTest, APinnedTabGoesBackToThePageItShowed) {
  SidebarNode* p = AddTab(today(), "p");
  model_.Move(p, pinned(), 0);
  ASSERT_EQ(p->saved_url(), GURL("https://p.test/"));
  model_.SetUrl(p, GURL("https://elsewhere.test/"));
  model_.SetTitle(p, u"Elsewhere");

  // Its tab closes: it goes back to its pinned URL.
  closed_.RecordUnload(model_, *p);
  closed_.SetOpen(p->id(), /*shown=*/true);
  model_.SetUrl(p, p->saved_url());

  EXPECT_EQ(Undo(), p);
  EXPECT_EQ(p->url(), GURL("https://elsewhere.test/"));
  EXPECT_EQ(p->title(), u"Elsewhere");
}

TEST_F(ClosedEntriesTest, TheShownTabReopensElseTheFirstOpenOne) {
  SidebarNode* a = AddTab(today(), "a");
  SidebarNode* b = AddTab(today(), "b");
  SidebarNode* c = AddTab(today(), "c");
  const base::Uuid b_id = b->id();
  const base::Uuid c_id = c->id();
  {
    ClosedEntries::ScopedStep step(closed_);
    for (SidebarNode* tab : {a, b, c}) {
      closed_.RecordRemoval(model_, *tab);
    }
    closed_.SetOpen(b_id, /*shown=*/false);
    closed_.SetOpen(c_id, /*shown=*/true);
    // Shown only because c closed before it.
    closed_.SetOpen(a->id(), /*shown=*/true);
    closed_.SetHistory(c_id, 42);
    for (SidebarNode* tab : {a, b, c}) {
      model_.Remove(tab);
    }
  }
  ASSERT_EQ(closed_.undo_steps_for_testing().back().history.at(c_id), 42);

  SidebarNode* reopen = Undo();
  ASSERT_TRUE(reopen);
  EXPECT_EQ(reopen->id(), c_id);
}

TEST_F(ClosedEntriesTest, NoTabReopensIfNoneWasOpen) {
  SidebarNode* a = AddTab(today(), "a");
  closed_.RecordRemoval(model_, *a);
  model_.Remove(a);

  EXPECT_EQ(Undo(), nullptr);
  EXPECT_THAT(Titles(today()), ElementsAre("a"));
}

TEST_F(ClosedEntriesTest, EachCloseIsAStepAndANewOneDropsTheRedos) {
  SidebarNode* a = AddTab(today(), "a");
  SidebarNode* b = AddTab(today(), "b");
  SidebarNode* c = AddTab(today(), "c");
  closed_.RecordRemoval(model_, *a);
  model_.Remove(a);
  closed_.RecordRemoval(model_, *b);
  model_.Remove(b);
  EXPECT_EQ(closed_.undo_steps_for_testing().size(), 2u);

  Undo();
  EXPECT_THAT(Titles(today()), ElementsAre("b", "c"));
  EXPECT_TRUE(closed_.CanRedo());

  // A redo's closes keep the other redos.
  {
    ClosedEntries::ScopedStep step(closed_, /*keep_redo=*/true);
    closed_.RecordRemoval(model_, *c);
  }
  EXPECT_TRUE(closed_.CanRedo());

  // Any other close drops them.
  closed_.RecordRemoval(model_, *today()->children()[0]);
  EXPECT_FALSE(closed_.CanRedo());
}

TEST_F(ClosedEntriesTest, KeepsTheLatestSteps) {
  for (size_t i = 0; i < ClosedEntries::kMaxSteps + 5; ++i) {
    SidebarNode* tab = AddTab(today(), "t" + base::NumberToString(i));
    closed_.RecordRemoval(model_, *tab);
    model_.Remove(tab);
  }
  EXPECT_EQ(closed_.undo_steps_for_testing().size(), ClosedEntries::kMaxSteps);
}

TEST_F(ClosedEntriesTest, ExpectedClosesAreTakenOnceAndDroppedOnUndo) {
  SidebarNode* a = AddTab(today(), "a");
  SidebarNode* b = AddTab(today(), "b");
  const base::Uuid a_id = a->id();
  closed_.ExpectClose(a_id);
  EXPECT_TRUE(closed_.TakeExpectedClose(a_id));
  EXPECT_FALSE(closed_.TakeExpectedClose(a_id));

  // A close that never came (beforeunload kept the tab) isn't expected
  // after its step is undone.
  closed_.RecordRemoval(model_, *b);
  closed_.ExpectClose(b->id());
  const base::Uuid b_id = b->id();
  closed_.TakeUndo();
  EXPECT_FALSE(closed_.TakeExpectedClose(b_id));
}

TEST_F(ClosedEntriesTest, FavoritesComeBackToTheFavorites) {
  SidebarNode* f = favorites_.AddTab(favorites_.pinned_root(), 0,
                                     GURL("https://f.test/"), u"f");
  closed_.RecordRemoval(favorites_, *f);
  favorites_.Remove(f);
  EXPECT_FALSE(closed_.undo_steps_for_testing().back().space_id.is_valid());

  Undo();
  EXPECT_THAT(Titles(favorites_.pinned_root()), ElementsAre("f"));
  EXPECT_TRUE(today()->children().empty());
}

}  // namespace
}  // namespace compass
