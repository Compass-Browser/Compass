// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_files.h"

#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude::files {
namespace {

class ClaudeFilesTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    folder_ = dir_.GetPath().AppendASCII("work");
    ASSERT_TRUE(base::CreateDirectory(folder_));
  }

  std::string Contents(const std::string& relative) {
    std::string text;
    EXPECT_TRUE(base::ReadFileToString(
        folder_.Append(base::FilePath::FromUTF8Unsafe(relative)), &text));
    return text;
  }

  base::ScopedTempDir dir_;
  base::FilePath folder_;
};

TEST_F(ClaudeFilesTest, FileNames) {
  EXPECT_FALSE(CheckFileName("notes.md").has_value());
  EXPECT_FALSE(CheckFileName("Résumé 2026.txt").has_value());
  EXPECT_TRUE(CheckFileName("").has_value());
  EXPECT_TRUE(CheckFileName("a/b.md").has_value());
  EXPECT_TRUE(CheckFileName("a\\b.md").has_value());
  EXPECT_TRUE(CheckFileName(".bashrc").has_value());
  EXPECT_TRUE(CheckFileName("..").has_value());
  EXPECT_TRUE(CheckFileName("bad\nname").has_value());
  EXPECT_TRUE(CheckFileName("trailing.").has_value());
  EXPECT_TRUE(CheckFileName(std::string(201, 'a')).has_value());
}

TEST_F(ClaudeFilesTest, PathsStayRelative) {
  EXPECT_EQ(JoinInFolder(folder_, "").value(), folder_);
  EXPECT_EQ(JoinInFolder(folder_, "a/b.md").value(),
            folder_.AppendASCII("a").AppendASCII("b.md"));
  EXPECT_FALSE(JoinInFolder(folder_, "/etc/passwd").has_value());
  EXPECT_FALSE(JoinInFolder(folder_, "../x").has_value());
  EXPECT_FALSE(JoinInFolder(folder_, "a/../../x").has_value());
  EXPECT_FALSE(JoinInFolder(folder_, "a//b").has_value());
  EXPECT_FALSE(JoinInFolder(folder_, "./a").has_value());
  EXPECT_FALSE(JoinInFolder(folder_, "C:/x").has_value());
}

TEST_F(ClaudeFilesTest, SavesWithoutReplacing) {
  base::expected<base::FilePath, std::string> first =
      SaveNewFile(folder_, "notes.md", "one");
  ASSERT_TRUE(first.has_value()) << first.error();
  EXPECT_EQ(first->BaseName().AsUTF8Unsafe(), "notes.md");
  base::expected<base::FilePath, std::string> second =
      SaveNewFile(folder_, "notes.md", "two");
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_EQ(second->BaseName().AsUTF8Unsafe(), "notes (1).md");
  EXPECT_EQ(Contents("notes.md"), "one");
  EXPECT_EQ(Contents("notes (1).md"), "two");
}

TEST_F(ClaudeFilesTest, WritesReadsAndLists) {
  base::expected<bool, std::string> created =
      WriteTextFile(folder_, "a/b.md", "hello");
  ASSERT_TRUE(created.has_value()) << created.error();
  EXPECT_FALSE(*created);
  base::expected<bool, std::string> replaced =
      WriteTextFile(folder_, "a/b.md", "hello again");
  ASSERT_TRUE(replaced.has_value()) << replaced.error();
  EXPECT_TRUE(*replaced);
  EXPECT_EQ(ReadTextFile(folder_, "a/b.md", 0).value(), "hello again");
  EXPECT_EQ(ReadTextFile(folder_, "a/b.md", 6).value(), "again");
  EXPECT_FALSE(ReadTextFile(folder_, "a/b.md", 100).has_value());
  EXPECT_FALSE(ReadTextFile(folder_, "missing.md", 0).has_value());
  EXPECT_FALSE(ReadTextFile(folder_, "a", 0).has_value());

  const std::string root = ListFolder(folder_, "").value();
  EXPECT_NE(root.find("1 entry"), std::string::npos) << root;
  EXPECT_NE(root.find("a/"), std::string::npos) << root;
  const std::string inside = ListFolder(folder_, "a").value();
  EXPECT_NE(inside.find("b.md  11 B"), std::string::npos) << inside;
  EXPECT_FALSE(ListFolder(folder_, "nope").has_value());
}

TEST_F(ClaudeFilesTest, LongFilesComeInParts) {
  const std::string text(kMaxReadChars + 10, 'x');
  ASSERT_TRUE(WriteTextFile(folder_, "long.txt", text).has_value());
  const std::string first = ReadTextFile(folder_, "long.txt", 0).value();
  EXPECT_NE(first.find("10 more characters (read again with offset 100000)"),
            std::string::npos);
  EXPECT_EQ(ReadTextFile(folder_, "long.txt", kMaxReadChars).value(),
            std::string(10, 'x'));
}

TEST_F(ClaudeFilesTest, OnlyText) {
  ASSERT_TRUE(base::WriteFile(folder_.AppendASCII("image.png"),
                              std::string("\x89PNG\0\0", 6)));
  base::expected<std::string, std::string> read =
      ReadTextFile(folder_, "image.png", 0);
  ASSERT_FALSE(read.has_value());
  EXPECT_NE(read.error().find("isn't text"), std::string::npos);
  EXPECT_FALSE(CheckContent("fine").has_value());
  EXPECT_TRUE(CheckContent(std::string(kMaxWriteChars + 1, 'x')).has_value());
}

#if BUILDFLAG(IS_POSIX)
// A link inside the folder can't lead out of it.
TEST_F(ClaudeFilesTest, LinksDontLeadOut) {
  const base::FilePath outside = dir_.GetPath().AppendASCII("outside");
  ASSERT_TRUE(base::CreateDirectory(outside));
  ASSERT_TRUE(base::WriteFile(outside.AppendASCII("secret.txt"), "secret"));
  ASSERT_TRUE(
      base::CreateSymbolicLink(outside, folder_.AppendASCII("link")));
  EXPECT_FALSE(ReadTextFile(folder_, "link/secret.txt", 0).has_value());
  EXPECT_FALSE(WriteTextFile(folder_, "link/new.txt", "x").has_value());
  EXPECT_FALSE(ListFolder(folder_, "link").has_value());
  EXPECT_FALSE(base::PathExists(outside.AppendASCII("new.txt")));
  // A link to a file inside is fine.
  ASSERT_TRUE(WriteTextFile(folder_, "real.txt", "inside").has_value());
  ASSERT_TRUE(base::CreateSymbolicLink(folder_.AppendASCII("real.txt"),
                                       folder_.AppendASCII("alias.txt")));
  EXPECT_EQ(ReadTextFile(folder_, "alias.txt", 0).value(), "inside");
}
#endif

}  // namespace
}  // namespace compass::claude::files
