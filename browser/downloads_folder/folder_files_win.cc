// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/downloads_folder/folder_files.h"

#include <windows.h>

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <string>

#include "base/files/file_util.h"
#include "base/win/scoped_co_mem.h"

namespace compass::downloads_folder {

base::FilePath SystemScreenshotFolder() {
  // Win+PrtScn and the Snipping Tool save to the Screenshots known folder,
  // which the user can move.
  base::win::ScopedCoMem<wchar_t> path;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Screenshots, KF_FLAG_DEFAULT,
                                     nullptr, &path))) {
    return base::FilePath(path.get());
  }
  base::win::ScopedCoMem<wchar_t> pictures;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT,
                                     nullptr, &pictures))) {
    return base::FilePath(pictures.get()).Append(L"Screenshots");
  }
  return base::FilePath();
}

bool IsScreenCapture(const base::FilePath& path) {
  return false;
}

void MarkAsScreenCapture(const base::FilePath& path) {}

std::optional<base::Time> AddedToFolderTime(const base::FilePath& path) {
  // A file's creation time is when it was made in this folder (a copy or a
  // download makes it anew).
  base::File::Info info;
  if (!base::GetFileInfo(path, &info) || info.creation_time.is_null()) {
    return std::nullopt;
  }
  return std::max(info.creation_time, info.last_modified);
}

bool MoveToTrash(const base::FilePath& path) {
  // SHFileOperation wants a list ending in two nulls.
  std::wstring from = path.value();
  from.push_back(L'\0');
  from.push_back(L'\0');
  SHFILEOPSTRUCTW operation = {};
  operation.wFunc = FO_DELETE;
  operation.pFrom = from.c_str();
  operation.fFlags =
      FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
  return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
}

}  // namespace compass::downloads_folder
