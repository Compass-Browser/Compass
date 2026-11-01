// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/common/user_data_dir_mac.h"

#include <unistd.h>

#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

class UserDataDirMacTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    app_support_ = temp_.GetPath().Append("Application Support");
    caches_ = temp_.GetPath().Append("Caches");
    ASSERT_TRUE(base::CreateDirectory(app_support_));
    ASSERT_TRUE(base::CreateDirectory(caches_));
  }

  base::FilePath Old() { return app_support_.Append("Chromium"); }
  base::FilePath New() { return app_support_.Append("Compass"); }

  // A "Chromium" folder as Compass left it, with saved absolute paths.
  void MakeOldCompassFolder() {
    ASSERT_TRUE(base::CreateDirectory(Old().Append("Default")));
    ASSERT_TRUE(base::WriteFile(Old().Append("Compass Spaces.json"), "{}"));
    const std::string old_path = Old().value();
    ASSERT_TRUE(base::WriteFile(
        Old().Append("Local State"),
        "{\"last\":\"" + old_path + "/Default\",\"other\":\"" + old_path +
            " 2/x\"}"));
    ASSERT_TRUE(base::WriteFile(
        Old().Append("Default").Append("Preferences"),
        "{\"sync\":\"" + old_path + "/Default/Compass Sync\"}"));
    ASSERT_TRUE(base::CreateDirectory(caches_.Append("Chromium")));
  }

  std::string Read(const base::FilePath& path) {
    std::string text;
    EXPECT_TRUE(base::ReadFileToString(path, &text));
    return text;
  }

  base::ScopedTempDir temp_;
  base::FilePath app_support_;
  base::FilePath caches_;
};

TEST_F(UserDataDirMacTest, NewInstallsUseCompass) {
  EXPECT_EQ(ChooseMacProductDirName(app_support_, caches_), "Compass");
  EXPECT_FALSE(base::PathExists(New()));
}

TEST_F(UserDataDirMacTest, MovesCompasssOldFolderAndItsPaths) {
  MakeOldCompassFolder();

  EXPECT_EQ(ChooseMacProductDirName(app_support_, caches_), "Compass");

  EXPECT_FALSE(base::PathExists(Old()));
  EXPECT_TRUE(base::PathExists(New().Append("Compass Spaces.json")));
  const std::string new_path = New().value();
  const std::string old_path = Old().value();
  // Paths into the folder follow it; a sibling folder's doesn't.
  EXPECT_EQ(Read(New().Append("Local State")),
            "{\"last\":\"" + new_path + "/Default\",\"other\":\"" + old_path +
                " 2/x\"}");
  EXPECT_EQ(Read(New().Append("Default").Append("Preferences")),
            "{\"sync\":\"" + new_path + "/Default/Compass Sync\"}");
  EXPECT_TRUE(base::PathExists(caches_.Append("Compass")));
  EXPECT_FALSE(base::PathExists(caches_.Append("Chromium")));
}

TEST_F(UserDataDirMacTest, LeavesAPlainChromiumAlone) {
  ASSERT_TRUE(base::CreateDirectory(Old().Append("Default")));
  ASSERT_TRUE(base::WriteFile(Old().Append("Local State"), "{}"));

  EXPECT_EQ(ChooseMacProductDirName(app_support_, caches_), "Compass");
  EXPECT_TRUE(base::PathExists(Old().Append("Local State")));
  EXPECT_FALSE(base::PathExists(New()));
}

TEST_F(UserDataDirMacTest, KeepsTheOldFolderWhileARunningCompassHoldsIt) {
  MakeOldCompassFolder();
  // This test's own process stands for the running browser.
  ASSERT_TRUE(base::CreateSymbolicLink(
      base::FilePath("host-" + base::NumberToString(getpid())),
      Old().Append("SingletonLock")));

  EXPECT_EQ(ChooseMacProductDirName(app_support_, caches_), "Chromium");
  EXPECT_TRUE(base::PathExists(Old().Append("Compass Spaces.json")));
  EXPECT_FALSE(base::PathExists(New()));
}

TEST_F(UserDataDirMacTest, AStaleLockDoesntStopTheMove) {
  MakeOldCompassFolder();
  // No process has this ID.
  ASSERT_TRUE(base::CreateSymbolicLink(base::FilePath("host-99999999"),
                                       Old().Append("SingletonLock")));

  EXPECT_EQ(ChooseMacProductDirName(app_support_, caches_), "Compass");
  EXPECT_TRUE(base::PathExists(New().Append("Compass Spaces.json")));
}

TEST_F(UserDataDirMacTest, AnExistingCompassFolderWins) {
  MakeOldCompassFolder();
  ASSERT_TRUE(base::CreateDirectory(New()));

  EXPECT_EQ(ChooseMacProductDirName(app_support_, caches_), "Compass");
  EXPECT_TRUE(base::PathExists(Old().Append("Compass Spaces.json")));
}

}  // namespace
}  // namespace compass
