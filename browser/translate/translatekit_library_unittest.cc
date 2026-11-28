// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/translate/translatekit_library.h"

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "components/on_device_translation/features.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

using on_device_translation::kTranslateKitBinaryPath;

// The test binary sits in the out dir, next to the library (a data_dep).
TEST(TranslateKitLibraryTest, FindsTheBuiltLibrary) {
  base::FilePath library = GetBundledTranslateKitLibrary();
  ASSERT_FALSE(library.empty());
  EXPECT_TRUE(base::PathExists(library));
  EXPECT_NE(library.BaseName().value().find(FILE_PATH_LITERAL("translatekit")),
            base::FilePath::StringType::npos);
}

TEST(TranslateKitLibraryTest, SetsTheSwitch) {
  base::CommandLine command_line(base::CommandLine::NO_PROGRAM);
  UseBundledTranslateKit(command_line);
  EXPECT_EQ(command_line.GetSwitchValuePath(kTranslateKitBinaryPath),
            GetBundledTranslateKitLibrary());
}

TEST(TranslateKitLibraryTest, KeepsTheUsersSwitch) {
  base::CommandLine command_line(base::CommandLine::NO_PROGRAM);
  const base::FilePath mine(FILE_PATH_LITERAL("/somewhere/libmine.so"));
  command_line.AppendSwitchPath(kTranslateKitBinaryPath, mine);
  UseBundledTranslateKit(command_line);
  EXPECT_EQ(command_line.GetSwitchValuePath(kTranslateKitBinaryPath), mine);
}

}  // namespace
}  // namespace compass
