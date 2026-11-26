// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/downloads_folder/folder_files.h"

#include <algorithm>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::downloads_folder {
namespace {

base::FilePath P(const char* name) {
  return base::FilePath::FromUTF8Unsafe(name);
}

FolderFile File(const char* name,
                int minutes_ago,
                bool screen_capture = false) {
  FolderFile file;
  file.path = P(name);
  file.added = base::Time::Now() - base::Minutes(minutes_ago);
  file.kind = KindForPath(file.path);
  file.is_screen_capture = screen_capture;
  return file;
}

std::vector<std::string> Names(const std::vector<FolderFile>& files) {
  std::vector<std::string> names;
  for (const FolderFile& file : files) {
    names.push_back(file.path.BaseName().AsUTF8Unsafe());
  }
  return names;
}

TEST(FolderFilesTest, KindByExtension) {
  EXPECT_EQ(FileKind::kImage, KindForPath(P("a.PNG")));
  EXPECT_EQ(FileKind::kImage, KindForPath(P("photo.heic")));
  EXPECT_EQ(FileKind::kVideo, KindForPath(P("Screen Recording.mov")));
  EXPECT_EQ(FileKind::kOther, KindForPath(P("report.pdf")));
  EXPECT_EQ(FileKind::kOther, KindForPath(P("README")));
}

TEST(FolderFilesTest, HidesMetadataAndPartialDownloads) {
  EXPECT_TRUE(IsListable(P("report.pdf")));
  EXPECT_TRUE(IsListable(P("Folder")));
  EXPECT_FALSE(IsListable(P(".DS_Store")));
  EXPECT_FALSE(IsListable(P(".localized")));
  EXPECT_FALSE(IsListable(P("Desktop.ini")));
  EXPECT_FALSE(IsListable(P("Unconfirmed 12345.crdownload")));
  EXPECT_FALSE(IsListable(P("movie.mp4.part")));
  EXPECT_FALSE(IsListable(P("archive.zip.download")));
}

TEST(FolderFilesTest, DownloadsTabSkipsTheBrowsersRowsAndScreenshots) {
  std::vector<FolderFile> listing = {
      File("new.zip", 1), File("Screenshot.png", 2, /*screen_capture=*/true),
      File("from-chrome.pdf", 3), File("airdrop.jpg", 4), File("old.txt", 5)};
  base::flat_set<base::FilePath> skip = {P("from-chrome.pdf")};

  EXPECT_EQ((std::vector<std::string>{"new.zip", "airdrop.jpg", "old.txt"}),
            Names(DownloadsTabFiles(listing, skip,
                                    /*hide_screen_captures=*/true, 10)));
  EXPECT_EQ(
      (std::vector<std::string>{"new.zip", "Screenshot.png", "airdrop.jpg"}),
      Names(DownloadsTabFiles(listing, skip, /*hide_screen_captures=*/false,
                              3)));
}

TEST(FolderFilesTest, ScreenshotsTabKeepsImagesAndVideos) {
  std::vector<FolderFile> listing = {
      File("Screenshot 1.png", 1, /*screen_capture=*/true),
      File("notes.txt", 2),
      File("Screen Recording.mov", 3, /*screen_capture=*/true),
      File("holiday.jpg", 4)};
  std::vector<std::string> names = Names(ScreenshotsTabFiles(listing, 10));
  if (kSystemMarksScreenCaptures) {
    // macOS: a Desktop's other pictures aren't screenshots.
    EXPECT_EQ((std::vector<std::string>{"Screenshot 1.png",
                                        "Screen Recording.mov"}),
              names);
  } else {
    EXPECT_EQ((std::vector<std::string>{"Screenshot 1.png",
                                        "Screen Recording.mov", "holiday.jpg"}),
              names);
  }
  EXPECT_EQ(1u, ScreenshotsTabFiles(listing, 1).size());
}

TEST(FolderFilesTest, ListsAFolder) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath older = dir.GetPath().Append(P("older.txt"));
  const base::FilePath newer = dir.GetPath().Append(P("newer.png"));
  ASSERT_TRUE(base::WriteFile(older, "a"));
  ASSERT_TRUE(base::WriteFile(newer, "bb"));
  ASSERT_TRUE(base::WriteFile(dir.GetPath().Append(P(".hidden")), "c"));
  ASSERT_TRUE(
      base::WriteFile(dir.GetPath().Append(P("x.crdownload")), "partial"));
  ASSERT_TRUE(base::CreateDirectory(dir.GetPath().Append(P("Folder"))));
  // Where the system records when a file arrived (macOS, Windows), it's now
  // for all three; make the order clear through the fallback too.
  const base::Time now = base::Time::Now();
  ASSERT_TRUE(base::TouchFile(older, now - base::Hours(2),
                              now - base::Hours(2)));

  std::vector<FolderFile> files = ListFolder(dir.GetPath());
  ASSERT_EQ(3u, files.size());
  std::vector<std::string> names = Names(files);
  EXPECT_TRUE(std::ranges::find(names, "Folder") != names.end());
  EXPECT_TRUE(std::ranges::find(names, "newer.png") != names.end());
  EXPECT_TRUE(std::ranges::find(names, "older.txt") != names.end());
  for (const FolderFile& file : files) {
    if (file.path == newer) {
      EXPECT_EQ(2, file.size);
      EXPECT_EQ(FileKind::kImage, file.kind);
      EXPECT_FALSE(file.is_screen_capture);
    }
    if (file.path.BaseName() == P("Folder")) {
      EXPECT_TRUE(file.is_directory);
    }
  }
}

TEST(FolderFilesTest, MarkedScreenCapturesAreRecognized) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath shot = dir.GetPath().Append(P("Screenshot.png"));
  ASSERT_TRUE(base::WriteFile(shot, "png"));
  EXPECT_FALSE(IsScreenCapture(shot));
  MarkAsScreenCapture(shot);
  EXPECT_EQ(kSystemMarksScreenCaptures, IsScreenCapture(shot));
  std::vector<FolderFile> files = ListFolder(dir.GetPath());
  ASSERT_EQ(1u, files.size());
  EXPECT_EQ(kSystemMarksScreenCaptures, files[0].is_screen_capture);
}

TEST(FolderFilesTest, ScreenshotFolderIsSomewhere) {
  EXPECT_FALSE(ScreenshotFolder().empty());
}

}  // namespace
}  // namespace compass::downloads_folder
