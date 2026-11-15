// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/detect.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::browser_import {
namespace {

class DetectTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dir_.CreateUniqueTempDir()); }
  void TearDown() override { SetRootsForTesting(std::nullopt); }

  base::FilePath Make(const std::string& name) {
    const base::FilePath path = dir_.GetPath().AppendASCII(name);
    EXPECT_TRUE(base::CreateDirectory(path));
    return path;
  }

  base::ScopedTempDir dir_;
};

TEST_F(DetectTest, FindsChromiumProfilesNamedAsInTheBrowser) {
  const base::FilePath chrome = Make("chrome");
  ASSERT_TRUE(base::CreateDirectory(chrome.AppendASCII("Default")));
  ASSERT_TRUE(base::CreateDirectory(chrome.AppendASCII("Profile 1")));
  ASSERT_TRUE(base::WriteFile(chrome.AppendASCII("Local State"), R"({
    "profile": {"info_cache": {
      "Default": {"name": "Home"},
      "Profile 1": {"name": "Work"},
      "Profile 9": {"name": "Gone"}
    }}
  })"));
  const base::FilePath edge = Make("edge");
  ASSERT_TRUE(base::CreateDirectory(edge.AppendASCII("Default")));
  ASSERT_TRUE(base::WriteFile(edge.AppendASCII("Local State"),
                              R"({"profile": {"info_cache": {
                                  "Default": {"name": "Profile 1"}}}})"));
  // A browser that isn't there.
  SetRootsForTesting(std::vector<BrowserRoot>{
      {Browser::kBrave, dir_.GetPath().AppendASCII("brave")},
      {Browser::kChrome, chrome},
      {Browser::kEdge, edge},
  });

  std::vector<SourceProfile> profiles = DetectProfiles();
  ASSERT_EQ(profiles.size(), 3u);
  EXPECT_EQ(profiles[0].browser, Browser::kChrome);
  EXPECT_EQ(profiles[0].id, "chrome:Default");
  EXPECT_EQ(profiles[0].name, u"Home");
  EXPECT_EQ(profiles[0].path, chrome.AppendASCII("Default"));
  EXPECT_EQ(profiles[0].root, chrome);
  EXPECT_TRUE(profiles[0].several);
  EXPECT_EQ(SpaceNameFor(profiles[0]), u"Chrome – Home");
  EXPECT_EQ(profiles[1].id, "chrome:Profile 1");
  EXPECT_EQ(profiles[2].browser, Browser::kEdge);
  EXPECT_FALSE(profiles[2].several);
  EXPECT_EQ(SpaceNameFor(profiles[2]), u"Edge");
  EXPECT_FALSE(IsRunning(profiles[0]));
}

}  // namespace
}  // namespace compass::browser_import
