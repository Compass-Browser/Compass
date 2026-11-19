// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/easels/easel_titles.h"

#include <memory>
#include <vector>

#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::easels {
namespace {

constexpr char kBoard[] = "0b8f5f3e-8a3c-4d5e-9f6a-1b2c3d4e5f60";
constexpr char kOther[] = "7d1e2f3a-4b5c-4d6e-8f70-81a2b3c4d5e6";

TEST(EaselTitlesTest, TabTitle) {
  sync_pb::CompassEasel easel;
  EXPECT_EQ(u"Untitled Easel", EaselTabTitle(easel));
  easel.set_title("Trip");
  EXPECT_EQ(u"Trip", EaselTabTitle(easel));
}

TEST(EaselTitlesTest, RenamesEntriesOfTheBoardOnly) {
  SpaceModel space;
  SidebarNode* folder = space.AddFolder(space.pinned_root(), 0, u"F");
  // Pinned: its pinned URL (saved_url) is the board too.
  SidebarNode* pinned =
      space.AddTab(folder, 0, GURL(EaselUrl(kBoard)), u"Old");
  SidebarNode* today =
      space.AddTab(space.today_root(), 0, GURL(EaselUrl(kBoard)), u"Old");
  SidebarNode* other =
      space.AddTab(space.today_root(), 1, GURL(EaselUrl(kOther)), u"Other");
  SidebarNode* page = space.AddTab(space.today_root(), 2,
                                   GURL("https://example.com/"), u"Example");
  ASSERT_EQ(GURL(EaselUrl(kBoard)), pinned->saved_url());

  EXPECT_EQ(2u, ApplyEaselTitle(space, kBoard, u"New"));
  EXPECT_EQ(u"New", pinned->title());
  EXPECT_EQ(u"New", pinned->saved_title());
  EXPECT_EQ(u"New", today->title());
  EXPECT_EQ(u"Other", other->title());
  EXPECT_EQ(u"Example", page->title());
  // Nothing left to change.
  EXPECT_EQ(0u, ApplyEaselTitle(space, kBoard, u"New"));
}

TEST(EaselTitlesTest, PinnedEntryOnAnotherPageKeepsThatPagesTitle) {
  SpaceModel space;
  SidebarNode* pinned = space.AddTab(space.pinned_root(), 0,
                                     GURL(EaselUrl(kBoard)), u"Old");
  space.SetUrl(pinned, GURL("https://example.com/"));
  space.SetTitle(pinned, u"Example");

  EXPECT_EQ(1u, ApplyEaselTitle(space, kBoard, u"New"));
  EXPECT_EQ(u"Example", pinned->title());
  EXPECT_EQ(u"New", pinned->saved_title());
}

TEST(EaselTitlesTest, UsersNameStaysInFront) {
  SpaceModel space;
  SidebarNode* tab = space.AddTab(space.today_root(), 0,
                                  GURL(EaselUrl(kBoard)), u"Old");
  space.SetCustomTitle(tab, u"Mine");
  ApplyEaselTitle(space, kBoard, u"New");
  EXPECT_EQ(u"New", tab->title());
  EXPECT_EQ(u"Mine", tab->custom_title());
}

TEST(EaselTitlesTest, FollowerRenamesOnStoreChanges) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto store = std::make_unique<EaselStore>(dir.GetPath().AppendASCII("E"));
  {
    base::RunLoop loop;
    store->WhenLoaded(loop.QuitClosure());
    loop.Run();
  }
  auto easel = store->CreateEasel("Trip");
  ASSERT_TRUE(easel.has_value());

  SpaceModel space;
  SidebarNode* tab = space.AddTab(space.pinned_root(), 0,
                                  GURL(EaselUrl(easel->uuid())), u"Stale");
  EaselTitleFollower follower(
      store.get(), base::BindRepeating(
                       [](SpaceModel* space) {
                         return std::vector<SpaceModel*>{space};
                       },
                       &space));
  // A loaded store is followed at once.
  EXPECT_EQ(u"Trip", tab->title());

  ASSERT_TRUE(store->RenameEasel(easel->uuid(), "Holiday"));
  EXPECT_EQ(u"Holiday", tab->title());
  EXPECT_EQ(u"Holiday", tab->saved_title());

  ASSERT_TRUE(store->RenameEasel(easel->uuid(), ""));
  EXPECT_EQ(u"Untitled Easel", tab->title());
}

}  // namespace
}  // namespace compass::easels
