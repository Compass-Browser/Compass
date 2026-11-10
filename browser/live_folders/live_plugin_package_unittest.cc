// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/live_folders/live_plugin_package.h"

#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

constexpr char kManifest[] = R"({"id": "com.example.feed", "name": "Feed",
  "version": "1.2", "api": 1, "hosts": ["example.com"]})";

TEST(LivePluginPackageTest, HashesEveryFile) {
  const std::string hash = LivePluginHash(kManifest, "entry", "");
  EXPECT_EQ(hash.size(), 64u);
  EXPECT_EQ(hash, LivePluginHash(kManifest, "entry", ""));
  EXPECT_NE(hash, LivePluginHash(kManifest, "entry2", ""));
  // Lengths are hashed: moving bytes between files changes it.
  EXPECT_NE(LivePluginHash("ab", "c", ""), LivePluginHash("a", "bc", ""));
}

TEST(LivePluginPackageTest, RefusesBuiltinIdsAndBadManifests) {
  EXPECT_FALSE(MakeLivePluginPackage(
                   GURL("https://x.test/manifest.json"),
                   R"({"id": "compass.mine", "name": "N", "version": "1",
                       "api": 1})",
                   "", "")
                   .has_value());
  EXPECT_FALSE(MakeLivePluginPackage(GURL("https://x.test/m.json"), "{",
                                     "", "")
                   .has_value());
  auto package = MakeLivePluginPackage(GURL("https://x.test/manifest.json"),
                                       kManifest, "async function f() {}", "");
  ASSERT_TRUE(package.has_value()) << package.error();
  EXPECT_EQ(package->manifest.id, "com.example.feed");
}

TEST(LivePluginPackageTest, WritesReadsAndDeletes) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto package = MakeLivePluginPackage(GURL("https://x.test/manifest.json"),
                                       kManifest, "// entry", "");
  ASSERT_TRUE(package.has_value());
  ASSERT_TRUE(WriteLivePluginPackage(dir.GetPath(), *package));
  EXPECT_TRUE(base::PathExists(dir.GetPath()
                                   .AppendASCII("com.example.feed")
                                   .AppendASCII("1.2")
                                   .AppendASCII("main.js")));

  std::vector<LivePluginPackage> read = ReadLivePluginPackages(dir.GetPath());
  ASSERT_EQ(read.size(), 1u);
  EXPECT_EQ(read[0].sha256, package->sha256);
  EXPECT_EQ(read[0].source, GURL("https://x.test/manifest.json"));

  EXPECT_TRUE(DeleteLivePluginPackage(dir.GetPath(), "com.example.feed"));
  EXPECT_TRUE(ReadLivePluginPackages(dir.GetPath()).empty());
}

TEST(LivePluginPackageTest, FilePathsStayInside) {
  const base::FilePath dir(FILE_PATH_LITERAL("/plugins"));
  EXPECT_FALSE(LivePluginFilePath(dir, "a/1/main.js").empty());
  EXPECT_TRUE(LivePluginFilePath(dir, "../1/main.js").empty());
  EXPECT_TRUE(LivePluginFilePath(dir, "a/../main.js").empty());
  EXPECT_TRUE(LivePluginFilePath(dir, "a/1/sub/main.js").empty());
  EXPECT_TRUE(LivePluginFilePath(dir, "a/1/..").empty());
  EXPECT_TRUE(LivePluginFilePath(dir, "a/1/").empty());
}

}  // namespace
}  // namespace compass
