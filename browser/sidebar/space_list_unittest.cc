// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/space_list.h"

#include <string>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

using ::testing::ElementsAre;

class Recorder : public SpaceList::Observer {
 public:
  void OnSpaceAdded(SpaceModel& space) override {
    events.push_back("added " + base::UTF16ToUTF8(space.name()));
  }
  void OnActiveSpaceChanged() override { events.push_back("active"); }
  void OnSpaceUpdated(const SpaceModel& space) override {
    events.push_back("updated " + base::UTF16ToUTF8(space.name()));
  }
  void OnSpaceWillBeRemoved(SpaceModel& space) override {
    events.push_back("removed " + base::UTF16ToUTF8(space.name()));
  }
  void OnSpaceMoved(const SpaceModel& space) override {
    events.push_back("moved " + base::UTF16ToUTF8(space.name()));
  }

  std::vector<std::string> events;
};

TEST(SpaceListTest, StartsWithOneActiveSpace) {
  SpaceList list;

  ASSERT_EQ(list.spaces().size(), 1u);
  EXPECT_EQ(&list.active_space(), list.spaces()[0].get());
  EXPECT_EQ(list.active_space().name(), u"Home");
}

TEST(SpaceListTest, AddsSpacesAndSwitchesTheActiveOne) {
  SpaceList list;
  Recorder recorder;
  list.AddObserver(&recorder);

  SpaceModel* work = list.AddSpace(0, u"Work");
  list.SetActiveSpace(*work);
  list.SetActiveSpace(*work);

  EXPECT_EQ(list.spaces()[0].get(), work);
  EXPECT_EQ(&list.active_space(), work);
  EXPECT_EQ(list.FindSpace(work->id()), work);
  EXPECT_THAT(recorder.events, ElementsAre("added Work", "active"));
  list.RemoveObserver(&recorder);
}

TEST(SpaceListTest, FindsTheSpaceOfANode) {
  SpaceList list;
  SpaceModel* work = list.AddSpace(1, u"Work");
  SidebarNode* tab =
      work->AddTab(work->today_root(), 0, GURL("https://a.test/"), u"A");

  EXPECT_EQ(list.FindSpaceOf(tab), work);
  EXPECT_EQ(list.FindSpaceOf(list.active_space().today_root()),
            &list.active_space());
}

TEST(SpaceListTest, RemovingTheActiveSpaceActivatesTheNextOne) {
  SpaceList list;
  SpaceModel* home = &list.active_space();
  SpaceModel* work = list.AddSpace(1, u"Work");
  SpaceModel* play = list.AddSpace(2, u"Play");
  Recorder recorder;
  list.AddObserver(&recorder);

  list.SetActiveSpace(*work);
  list.RemoveSpace(*work);
  EXPECT_EQ(&list.active_space(), play);
  // The last one gives way to the one before it.
  list.RemoveSpace(*play);
  EXPECT_EQ(&list.active_space(), home);

  ASSERT_EQ(list.spaces().size(), 1u);
  EXPECT_THAT(recorder.events,
              ElementsAre("active", "active", "removed Work", "active",
                          "removed Play"));
  list.RemoveObserver(&recorder);
}

TEST(SpaceListTest, ReportsChangesToAnySpace) {
  SpaceList list;
  SpaceModel* work = list.AddSpace(1, u"Work");
  Recorder recorder;
  list.AddObserver(&recorder);

  work->SetName(u"Job");
  work->SetColor(SpaceColor::kBlue);
  list.active_space().SetName(u"House");

  EXPECT_THAT(recorder.events,
              ElementsAre("updated Job", "updated Job", "updated House"));
  list.RemoveObserver(&recorder);
}

TEST(SpaceListTest, RemovingAnotherSpaceKeepsTheActiveOne) {
  SpaceList list;
  SpaceModel* home = &list.active_space();
  SpaceModel* work = list.AddSpace(0, u"Work");

  list.RemoveSpace(*work);

  ASSERT_EQ(list.spaces().size(), 1u);
  EXPECT_EQ(&list.active_space(), home);
  EXPECT_EQ(list.FindSpace(home->id()), home);
}

TEST(SpaceListTest, MovesSpacesBySlot) {
  SpaceList list;
  SpaceModel* home = &list.active_space();
  SpaceModel* work = list.AddSpace(1, u"Work");
  SpaceModel* play = list.AddSpace(2, u"Play");
  Recorder recorder;
  list.AddObserver(&recorder);
  auto spaces = [&] {
    std::vector<SpaceModel*> result;
    for (const auto& space : list.spaces()) {
      result.push_back(space.get());
    }
    return result;
  };

  // Slots are counted with the space in place: the ones next to it are no-ops.
  list.MoveSpace(*work, 1);
  list.MoveSpace(*work, 2);
  EXPECT_THAT(spaces(), ElementsAre(home, work, play));

  list.MoveSpace(*home, 3);
  EXPECT_THAT(spaces(), ElementsAre(work, play, home));
  list.MoveSpace(*home, 0);
  EXPECT_THAT(spaces(), ElementsAre(home, work, play));
  list.MoveSpace(*work, 100);
  EXPECT_THAT(spaces(), ElementsAre(home, play, work));

  EXPECT_EQ(&list.active_space(), home);
  EXPECT_THAT(recorder.events,
              ElementsAre("moved Home", "moved Home", "moved Work"));
  list.RemoveObserver(&recorder);
}

}  // namespace
}  // namespace compass
