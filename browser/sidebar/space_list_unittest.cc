// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/sidebar/space_list.h"

#include <string>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace strata {
namespace {

using ::testing::ElementsAre;

class Recorder : public SpaceList::Observer {
 public:
  void OnSpaceAdded(SpaceModel& space) override {
    events.push_back("added " + base::UTF16ToUTF8(space.name()));
  }
  void OnActiveSpaceChanged() override { events.push_back("active"); }

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

}  // namespace
}  // namespace strata
