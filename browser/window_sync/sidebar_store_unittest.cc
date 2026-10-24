// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/sidebar_store.h"

#include <memory>
#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "strata/browser/sidebar/space_model.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace strata {
namespace {

// Titles of |parent|'s children, comma separated.
std::string Titles(const SidebarNode* parent) {
  std::string titles;
  for (const auto& child : parent->children()) {
    titles += (titles.empty() ? "" : ",") + base::UTF16ToUTF8(child->title());
  }
  return titles;
}

class SidebarStoreTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dir_.CreateUniqueTempDir()); }

  base::FilePath path() const { return dir_.GetPath().AppendASCII("s.json"); }

  void Load(SidebarStore& store) {
    base::RunLoop run_loop;
    store.Load(run_loop.QuitClosure());
    run_loop.Run();
  }

  // Destroys |store|, which flushes, and waits for the write.
  void Close(std::unique_ptr<SidebarStore> store) {
    store.reset();
    task_environment_.RunUntilIdle();
  }

  void AddPinned(SpaceModel& model, const std::string& title) {
    model.AddTab(model.pinned_root(), model.pinned_root()->children().size(),
                 GURL("https://" + title + ".test/"), base::UTF8ToUTF16(title));
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir dir_;
};

TEST_F(SidebarStoreTest, SavesPinnedAndLoadsItBack) {
  SpaceModel model;
  auto store = std::make_unique<SidebarStore>(path(), &model);
  Load(*store);
  AddPinned(model, "a");
  model.AddFolder(model.pinned_root(), 1, u"Work");
  Close(std::move(store));

  SpaceModel restored;
  SidebarStore restored_store(path(), &restored);
  Load(restored_store);

  EXPECT_EQ(Titles(restored.pinned_root()), "a,Work");
}

TEST_F(SidebarStoreTest, SavesTodayAndLoadsItBack) {
  SpaceModel model;
  auto store = std::make_unique<SidebarStore>(path(), &model);
  Load(*store);
  model.AddTab(model.today_root(), 0, GURL("https://t.test/"), u"t");
  model.AddTab(model.today_root(), 1, GURL("chrome://newtab/"), u"ntp");
  Close(std::move(store));

  SpaceModel restored;
  SidebarStore restored_store(path(), &restored);
  Load(restored_store);

  EXPECT_EQ(Titles(restored.today_root()), "t");
}

TEST_F(SidebarStoreTest, NothingIsWrittenBeforeTheLoad) {
  {
    SpaceModel model;
    auto store = std::make_unique<SidebarStore>(path(), &model);
    Load(*store);
    AddPinned(model, "saved");
    Close(std::move(store));
  }

  // Closed before loading: the change must not replace the saved entries.
  SpaceModel model;
  auto store = std::make_unique<SidebarStore>(path(), &model);
  AddPinned(model, "early");
  Close(std::move(store));

  SpaceModel restored;
  SidebarStore restored_store(path(), &restored);
  Load(restored_store);
  EXPECT_EQ(Titles(restored.pinned_root()), "saved");
}

TEST_F(SidebarStoreTest, KeepsChangesMadeBeforeTheLoad) {
  {
    SpaceModel model;
    auto store = std::make_unique<SidebarStore>(path(), &model);
    Load(*store);
    AddPinned(model, "saved");
    Close(std::move(store));
  }

  SpaceModel model;
  auto store = std::make_unique<SidebarStore>(path(), &model);
  AddPinned(model, "early");
  Load(*store);
  EXPECT_EQ(Titles(model.pinned_root()), "saved,early");
  Close(std::move(store));

  SpaceModel restored;
  SidebarStore restored_store(path(), &restored);
  Load(restored_store);
  EXPECT_EQ(Titles(restored.pinned_root()), "saved,early");
}

}  // namespace
}  // namespace strata
