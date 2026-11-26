// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/downloads_folder/folder_files.h"

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/nix/xdg_util.h"
#include "base/process/launch.h"
#include "base/process/process.h"

namespace compass::downloads_folder {

base::FilePath SystemScreenshotFolder() {
  // GNOME and KDE's Spectacle save to Pictures/Screenshots.
  return base::nix::GetXDGUserDirectory("PICTURES", "Pictures")
      .Append("Screenshots");
}

bool IsScreenCapture(const base::FilePath& path) {
  return false;
}

void MarkAsScreenCapture(const base::FilePath& path) {}

std::optional<base::Time> AddedToFolderTime(const base::FilePath& path) {
  return std::nullopt;
}

bool MoveToTrash(const base::FilePath& path) {
  // GLib's trash, which follows the freedesktop.org spec (the file can be
  // put back from the file manager).
  base::CommandLine command(base::FilePath("gio"));
  command.AppendArg("trash");
  command.AppendArg("--");
  command.AppendArgPath(path);
  base::LaunchOptions options;
  options.allow_new_privs = true;
  base::Process process = base::LaunchProcess(command, options);
  int exit_code = -1;
  return process.IsValid() && process.WaitForExit(&exit_code) &&
         exit_code == 0;
}

}  // namespace compass::downloads_folder
