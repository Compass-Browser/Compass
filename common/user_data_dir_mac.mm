// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/common/user_data_dir_mac.h"

#import <Foundation/Foundation.h>
#include <errno.h>
#include <signal.h>

#include <string>

#include "base/apple/foundation_util.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"

namespace compass {

namespace {

constexpr char kNewName[] = "Compass";
constexpr char kOldName[] = "Chromium";

// Files only Compass writes in its user data folder.
bool IsCompassFolder(const base::FilePath& dir) {
  return base::PathExists(dir.Append("Compass Spaces.json")) ||
         base::PathExists(dir.Append("Strata Spaces.json"));
}

// Whether a running browser holds |dir|: its SingletonLock is a link to
// "<host>-<pid>", and that process is alive.
bool IsInUse(const base::FilePath& dir) {
  base::FilePath target;
  if (!base::ReadSymbolicLink(dir.Append("SingletonLock"), &target)) {
    return false;
  }
  const std::string value = target.value();
  const size_t dash = value.rfind('-');
  int pid = 0;
  if (dash == std::string::npos ||
      !base::StringToInt(value.substr(dash + 1), &pid) || pid <= 0) {
    return false;
  }
  return kill(pid, 0) == 0 || errno == EPERM;
}

// Replaces |from| with |to| in a JSON file's text, where it's followed by the
// end of the string or a path separator.
void RewritePaths(const base::FilePath& file,
                  const std::string& from,
                  const std::string& to) {
  std::string text;
  if (!base::ReadFileToString(file, &text)) {
    return;
  }
  std::string out;
  size_t last = 0;
  bool changed = false;
  for (size_t pos = text.find(from); pos != std::string::npos;
       pos = text.find(from, pos + from.size())) {
    const size_t end = pos + from.size();
    if (end < text.size() && text[end] != '"' && text[end] != '/') {
      continue;
    }
    out.append(text, last, pos - last);
    out.append(to);
    last = end;
    changed = true;
  }
  if (!changed) {
    return;
  }
  out.append(text, last, std::string::npos);
  base::WriteFile(file, out);
}

}  // namespace

std::string ChooseMacProductDirName(const base::FilePath& app_support,
                                    const base::FilePath& caches) {
  const base::FilePath new_dir = app_support.Append(kNewName);
  const base::FilePath old_dir = app_support.Append(kOldName);
  if (base::PathExists(new_dir) || !IsCompassFolder(old_dir)) {
    return kNewName;
  }
  if (IsInUse(old_dir) || !base::Move(old_dir, new_dir)) {
    return kOldName;
  }

  // Saved absolute paths into the folder (the sync engine's folder, speech
  // models...). Secure Preferences is left alone: its values are signed.
  RewritePaths(new_dir.Append("Local State"), old_dir.value(),
               new_dir.value());
  base::FileEnumerator profiles(new_dir, /*recursive=*/false,
                                base::FileEnumerator::DIRECTORIES);
  for (base::FilePath profile = profiles.Next(); !profile.empty();
       profile = profiles.Next()) {
    RewritePaths(profile.Append("Preferences"), old_dir.value(),
                 new_dir.value());
  }

  // The cache follows; it's only a cache, so a failure costs nothing.
  const base::FilePath old_cache = caches.Append(kOldName);
  const base::FilePath new_cache = caches.Append(kNewName);
  if (base::PathExists(old_cache) && !base::PathExists(new_cache)) {
    base::Move(old_cache, new_cache);
  }
  return kNewName;
}

const char* GetMacProductDirName() {
  static const base::NoDestructor<std::string> name([] {
    base::FilePath app_support;
    base::FilePath caches;
    if (!base::apple::GetUserDirectory(NSApplicationSupportDirectory,
                                       &app_support) ||
        !base::apple::GetUserDirectory(NSCachesDirectory, &caches)) {
      return std::string(kNewName);
    }
    return ChooseMacProductDirName(app_support, caches);
  }());
  return name->c_str();
}

}  // namespace compass
