// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/arc_paths.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::arc {
namespace {

TEST(ArcPathsTest, Mac) {
  const base::FilePath library(FILE_PATH_LITERAL("/Users/a/Library"));
  const ArcPaths paths =
      MacArcPaths(library.AppendASCII("Application Support"),
                  library.AppendASCII("Caches"));
  const base::FilePath arc =
      library.AppendASCII("Application Support").AppendASCII("Arc");
  EXPECT_EQ(paths.user_data, arc.AppendASCII("User Data"));
  EXPECT_EQ(paths.sidebar, arc.AppendASCII("StorableSidebar.json"));
  EXPECT_EQ(paths.archive, arc.AppendASCII("StorableArchiveItems.json"));
  EXPECT_EQ(paths.cache, library.AppendASCII("Caches")
                             .AppendASCII("Arc")
                             .AppendASCII("User Data"));
}

class WindowsArcPathsTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dir_.CreateUniqueTempDir()); }

  // %LOCALAPPDATA%.
  const base::FilePath& local() const { return dir_.GetPath(); }

  // The Arc folder inside the package folder |package|.
  base::FilePath ArcIn(std::string_view package) const {
    return local()
        .AppendASCII("Packages")
        .AppendASCII(package)
        .AppendASCII("LocalCache")
        .AppendASCII("Local")
        .AppendASCII("Arc");
  }

  base::ScopedTempDir dir_;
};

TEST_F(WindowsArcPathsTest, FindsThePackagesArcFolder) {
  const base::FilePath arc = ArcIn("TheBrowserCompany.Arc_ttt1ap7aakyb4");
  ASSERT_TRUE(base::CreateDirectory(arc.AppendASCII("User Data")));
  std::optional<ArcPaths> paths = FindWindowsArcPaths(local());
  ASSERT_TRUE(paths);
  EXPECT_EQ(paths->user_data, arc.AppendASCII("User Data"));
  EXPECT_EQ(paths->sidebar, arc.AppendASCII("StorableSidebar.json"));
  EXPECT_EQ(paths->archive, arc.AppendASCII("StorableArchiveItems.json"));
  // The cache is in each profile folder.
  EXPECT_TRUE(paths->cache.empty());
}

TEST_F(WindowsArcPathsTest, SkipsOtherPackagesAndEmptyOnes) {
  // Another app, a package that only looks like Arc's, and one of Arc's
  // without data (sorting first): the one with User Data counts.
  ASSERT_TRUE(base::CreateDirectory(
      ArcIn("Other.App_abc").AppendASCII("User Data")));
  ASSERT_TRUE(base::CreateDirectory(
      ArcIn("TheBrowserCompany.ArcSearch_abc").AppendASCII("User Data")));
  ASSERT_TRUE(base::CreateDirectory(ArcIn("TheBrowserCompany.Arc_aaaa")));
  EXPECT_FALSE(FindWindowsArcPaths(local()));

  const base::FilePath arc = ArcIn("TheBrowserCompany.Arc_ttt1ap7aakyb4");
  ASSERT_TRUE(base::CreateDirectory(arc.AppendASCII("User Data")));
  std::optional<ArcPaths> paths = FindWindowsArcPaths(local());
  ASSERT_TRUE(paths);
  EXPECT_EQ(paths->user_data, arc.AppendASCII("User Data"));
}

TEST_F(WindowsArcPathsTest, NothingWithoutPackages) {
  EXPECT_FALSE(FindWindowsArcPaths(local()));
  EXPECT_FALSE(FindWindowsArcPaths(base::FilePath()));
}

}  // namespace
}  // namespace compass::arc
