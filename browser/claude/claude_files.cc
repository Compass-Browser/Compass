// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_files.h"

#include <algorithm>
#include <string>
#include <vector>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/strings/stringprintf.h"
#include "base/types/expected_macros.h"
#include "build/build_config.h"

namespace compass::claude::files {

namespace {

// Characters (code points) in UTF-8 text.
size_t CountChars(std::string_view text) {
  return std::ranges::count_if(
      text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; });
}

std::vector<std::string_view> SplitParts(std::string_view relative) {
#if BUILDFLAG(IS_WIN)
  constexpr std::string_view kSeparators = "/\\";
#else
  constexpr std::string_view kSeparators = "/";
#endif
  return base::SplitStringPiece(relative, kSeparators, base::KEEP_WHITESPACE,
                                base::SPLIT_WANT_ALL);
}

std::string FormatSize(int64_t bytes) {
  if (bytes < 1024) {
    return base::StrCat({base::NumberToString(bytes), " B"});
  }
  if (bytes < 1024 * 1024) {
    return base::StrCat(
        {base::NumberToString((bytes * 10 / 1024) / 10.0), " KB"});
  }
  return base::StrCat(
      {base::NumberToString((bytes * 10 / (1024 * 1024)) / 10.0), " MB"});
}

// "2026-11-12 21:40", local time.
std::string FormatTime(base::Time time) {
  base::Time::Exploded exploded;
  time.LocalExplode(&exploded);
  return base::StringPrintf("%04d-%02d-%02d %02d:%02d", exploded.year,
                            exploded.month, exploded.day_of_month,
                            exploded.hour, exploded.minute);
}

// Real path of |path|'s nearest existing part, with what doesn't exist yet
// appended.
std::optional<base::FilePath> RealPathForChecking(const base::FilePath& path) {
  std::vector<base::FilePath::StringType> missing;
  base::FilePath existing = path;
  while (!base::PathExists(existing)) {
    const base::FilePath parent = existing.DirName();
    if (parent == existing) {
      return std::nullopt;
    }
    missing.push_back(existing.BaseName().value());
    existing = parent;
  }
  base::FilePath real = base::MakeAbsoluteFilePath(existing);
  if (real.empty()) {
    return std::nullopt;
  }
  for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
    real = real.Append(*it);
  }
  return real;
}

}  // namespace

std::optional<std::string> CheckFileName(std::string_view name) {
  if (name.empty()) {
    return "The file needs a name.";
  }
  if (CountChars(name) > kMaxNameChars) {
    return "The name is too long (at most 200 characters).";
  }
  if (!base::IsStringUTF8(name)) {
    return "The name isn't valid text.";
  }
  if (name.find_first_of("/\\:") != std::string_view::npos) {
    return "Give a file name only, without folders.";
  }
  if (name.front() == '.' || name == "..") {
    return "The name can't start with a dot.";
  }
  if (std::ranges::any_of(name, [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
      })) {
    return "The name has control characters.";
  }
  if (name.back() == ' ' || name.back() == '.') {
    return "The name can't end with a space or a dot.";
  }
  return std::nullopt;
}

base::expected<base::FilePath, std::string> JoinInFolder(
    const base::FilePath& folder,
    std::string_view relative) {
  if (!base::IsStringUTF8(relative)) {
    return base::unexpected("The path isn't valid text.");
  }
  if (relative.empty()) {
    return folder;
  }
  if (relative.front() == '/' || relative.front() == '\\' ||
      (relative.size() >= 2 && relative[1] == ':')) {
    return base::unexpected(
        "Give a path relative to the working folder, like \"notes/a.md\".");
  }
  base::FilePath path = folder;
  for (std::string_view part : SplitParts(relative)) {
    if (part.empty() || part == "." || part == "..") {
      return base::unexpected(
          "The path can't have empty, \".\" or \"..\" parts: files outside "
          "the working folder can't be used.");
    }
    if (std::ranges::any_of(part, [](char c) {
          return static_cast<unsigned char>(c) < 0x20;
        })) {
      return base::unexpected("The path has control characters.");
    }
    path = path.Append(base::FilePath::FromUTF8Unsafe(part));
  }
  return path;
}

std::optional<std::string> CheckContent(std::string_view content) {
  if (!base::IsStringUTF8(content)) {
    return "The content isn't valid text.";
  }
  if (CountChars(content) > kMaxWriteChars) {
    return "The content is too long (at most 1,000,000 characters).";
  }
  return std::nullopt;
}

base::expected<void, std::string> CheckStaysInFolder(
    const base::FilePath& folder,
    const base::FilePath& path) {
  const base::FilePath real_folder = base::MakeAbsoluteFilePath(folder);
  if (real_folder.empty() || !base::DirectoryExists(real_folder)) {
    return base::unexpected(base::StrCat(
        {"The folder ", folder.AsUTF8Unsafe(),
         " isn't there anymore. The user can choose another in Compass's "
         "Claude settings."}));
  }
  const std::optional<base::FilePath> real = RealPathForChecking(path);
  if (!real || (*real != real_folder && !real_folder.IsParent(*real))) {
    return base::unexpected(
        "That path leads outside the working folder (a link), so it can't be "
        "used.");
  }
  return base::ok();
}

base::expected<bool, std::string> FileExists(const base::FilePath& path) {
  if (base::DirectoryExists(path)) {
    return base::unexpected("That's a folder, not a file.");
  }
  return base::PathExists(path);
}

base::expected<base::FilePath, std::string> SaveNewFile(
    const base::FilePath& folder,
    std::string_view name,
    std::string_view content) {
  if (!base::DirectoryExists(folder) && !base::CreateDirectory(folder)) {
    return base::unexpected(base::StrCat(
        {"The folder ", folder.AsUTF8Unsafe(), " can't be made."}));
  }
  RETURN_IF_ERROR(CheckStaysInFolder(folder, folder));
  const base::FilePath wanted =
      folder.Append(base::FilePath::FromUTF8Unsafe(name));
  const base::FilePath path = base::GetUniquePath(wanted);
  if (path.empty()) {
    return base::unexpected("Too many files have that name already.");
  }
  if (!base::WriteFile(path, content)) {
    return base::unexpected(
        base::StrCat({"The file couldn't be written in ",
                      folder.AsUTF8Unsafe(), "."}));
  }
  return path;
}

base::expected<std::string, std::string> ListFolder(
    const base::FilePath& folder,
    std::string_view relative) {
  ASSIGN_OR_RETURN(const base::FilePath dir, JoinInFolder(folder, relative));
  RETURN_IF_ERROR(CheckStaysInFolder(folder, dir));
  if (!base::DirectoryExists(dir)) {
    return base::unexpected(base::StrCat(
        {"There's no folder ", DisplayPath(relative), " in the working "
                                                      "folder."}));
  }
  struct Entry {
    std::string name;
    bool folder = false;
    int64_t size = 0;
    base::Time modified;
  };
  std::vector<Entry> entries;
  size_t total = 0;
  base::FileEnumerator enumerator(
      dir, /*recursive=*/false,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    ++total;
    const base::FileEnumerator::FileInfo info = enumerator.GetInfo();
    entries.push_back({path.BaseName().AsUTF8Unsafe(), info.IsDirectory(),
                       info.GetSize(), info.GetLastModifiedTime()});
  }
  std::ranges::sort(entries, {}, &Entry::name);
  if (entries.size() > kMaxEntries) {
    entries.resize(kMaxEntries);
  }
  std::string text = base::StrCat(
      {relative.empty() ? std::string("The working folder")
                        : DisplayPath(relative),
       " has ", base::NumberToString(total),
       total == 1 ? " entry" : " entries",
       total > kMaxEntries ? " (the first 500 by name)" : "",
       total ? ":\n" : "."});
  for (const Entry& entry : entries) {
    base::StrAppend(&text, {entry.name, entry.folder ? "/" : "", "  "});
    if (!entry.folder) {
      base::StrAppend(&text, {FormatSize(entry.size), "  "});
    }
    base::StrAppend(&text, {FormatTime(entry.modified), "\n"});
  }
  return base::ok(text);
}

base::expected<std::string, std::string> ReadTextFile(
    const base::FilePath& folder,
    std::string_view relative,
    size_t offset) {
  ASSIGN_OR_RETURN(const base::FilePath path, JoinInFolder(folder, relative));
  if (relative.empty()) {
    return base::unexpected("read_file needs a file's path.");
  }
  RETURN_IF_ERROR(CheckStaysInFolder(folder, path));
  ASSIGN_OR_RETURN(const bool exists, FileExists(path));
  if (!exists) {
    return base::unexpected(base::StrCat(
        {"There's no file ", DisplayPath(relative), " in the working "
                                                    "folder."}));
  }
  std::optional<int64_t> size = base::GetFileSize(path);
  if (!size) {
    return base::unexpected("The file can't be read.");
  }
  if (*size > kMaxReadBytes) {
    return base::unexpected(base::StrCat(
        {"The file is too large to read (", FormatSize(*size), ")."}));
  }
  std::string bytes;
  if (!base::ReadFileToString(path, &bytes)) {
    return base::unexpected("The file can't be read.");
  }
  if (bytes.find('\0') != std::string::npos || !base::IsStringUTF8(bytes)) {
    return base::unexpected(base::StrCat(
        {"The file isn't text (", FormatSize(*size),
         "), so read_file can't show it."}));
  }
  const std::u16string text = base::UTF8ToUTF16(bytes);
  if (offset > text.size()) {
    return base::unexpected(base::StrCat(
        {"The file has only ", base::NumberToString(text.size()),
         " characters."}));
  }
  const size_t length = std::min(kMaxReadChars, text.size() - offset);
  std::string result = base::UTF16ToUTF8(text.substr(offset, length));
  const size_t rest = text.size() - offset - length;
  if (rest) {
    base::StrAppend(&result,
                    {"\n…", base::NumberToString(rest),
                     " more characters (read again with offset ",
                     base::NumberToString(offset + length), ")."});
  }
  return base::ok(result);
}

base::expected<bool, std::string> WriteTextFile(const base::FilePath& folder,
                                                std::string_view relative,
                                                std::string_view content) {
  ASSIGN_OR_RETURN(const base::FilePath path, JoinInFolder(folder, relative));
  if (relative.empty()) {
    return base::unexpected("write_file needs a file's path.");
  }
  RETURN_IF_ERROR(CheckStaysInFolder(folder, path));
  ASSIGN_OR_RETURN(const bool existed, FileExists(path));
  if (!base::CreateDirectory(path.DirName())) {
    return base::unexpected("The folders on the way can't be made.");
  }
  // Made since the check: still inside.
  RETURN_IF_ERROR(CheckStaysInFolder(folder, path));
  if (!base::ImportantFileWriter::WriteFileAtomically(path, content)) {
    return base::unexpected("The file couldn't be written.");
  }
  return existed;
}

std::string DisplayPath(std::string_view relative) {
  std::string path(relative);
#if BUILDFLAG(IS_WIN)
  std::ranges::replace(path, '\\', '/');
#endif
  return path;
}

}  // namespace compass::claude::files
