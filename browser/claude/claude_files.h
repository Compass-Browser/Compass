// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_FILES_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_FILES_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/types/expected.h"

// The file tools' work (docs/design/claude.md, "Files"): save_file,
// list_files, read_file and write_file inside one folder. Everything but the
// checks named "Lexical" blocks: run it on a MayBlock sequence.
namespace compass::claude::files {

// What a file may hold, in characters, and what read_file gives at once.
inline constexpr size_t kMaxWriteChars = 1'000'000;
inline constexpr size_t kMaxReadChars = 100'000;
// list_files' entries.
inline constexpr size_t kMaxEntries = 500;
// Larger files aren't read.
inline constexpr int64_t kMaxReadBytes = 16 * 1024 * 1024;
// save_file's name.
inline constexpr size_t kMaxNameChars = 200;

// Lexical: why |name| can't be save_file's name (separators, "." or "..",
// a leading dot, empty, too long, control characters), or nullopt.
std::optional<std::string> CheckFileName(std::string_view name);

// Lexical: |relative| as path components under |folder|, or why not: an
// absolute path, a drive letter, a "." / ".." / empty part. An empty
// |relative| is |folder| itself.
base::expected<base::FilePath, std::string> JoinInFolder(
    const base::FilePath& folder,
    std::string_view relative);

// Why |content| is too long to write, or nullopt.
std::optional<std::string> CheckContent(std::string_view content);

// Blocking: |path| (from JoinInFolder()) once symlinks are resolved, for its
// nearest existing part, is still inside |folder|'s real path; the folder
// must exist.
base::expected<void, std::string> CheckStaysInFolder(
    const base::FilePath& folder,
    const base::FilePath& path);

// Blocking: whether |path| is an existing file (a folder is an error).
base::expected<bool, std::string> FileExists(const base::FilePath& path);

// Blocking: saves |content| as a new file named |name| in |folder|, never
// replacing one ("notes (1).md"). Returns the file's path.
base::expected<base::FilePath, std::string> SaveNewFile(
    const base::FilePath& folder,
    std::string_view name,
    std::string_view content);

// Blocking: list_files' text for |relative| in |folder|.
base::expected<std::string, std::string> ListFolder(
    const base::FilePath& folder,
    std::string_view relative);

// Blocking: read_file's text for |relative| from |offset| (characters).
base::expected<std::string, std::string> ReadTextFile(
    const base::FilePath& folder,
    std::string_view relative,
    size_t offset);

// Blocking: write_file. Makes the folders on the way and replaces a file
// atomically. Returns whether it replaced one.
base::expected<bool, std::string> WriteTextFile(const base::FilePath& folder,
                                                std::string_view relative,
                                                std::string_view content);

// "a/b.md" as the user and Claude see a path relative to the folder.
std::string DisplayPath(std::string_view relative);

}  // namespace compass::claude::files

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_FILES_H_
