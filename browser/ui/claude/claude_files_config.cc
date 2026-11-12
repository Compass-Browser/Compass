// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/claude_files_config.h"

#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/claude/claude_code.h"
#include "compass/browser/claude/claude_prefs.h"
#include "components/prefs/pref_service.h"

namespace compass::claude {

FilesConfig GetFilesConfig(Profile* profile) {
  const PrefService* prefs = profile->GetPrefs();
  FilesConfig config;
  switch (prefs->GetInteger(prefs::kFilesAccess)) {
    case static_cast<int>(prefs::FilesAccess::kSave):
      config.access = prefs::FilesAccess::kSave;
      break;
    case static_cast<int>(prefs::FilesAccess::kFolder):
      config.access = prefs::FilesAccess::kFolder;
      break;
    default:
      config.access = prefs::FilesAccess::kOff;
      return config;
  }
  config.folder =
      base::FilePath::FromUTF8Unsafe(prefs->GetString(prefs::kFilesFolder));
  if (config.access == prefs::FilesAccess::kSave && config.folder.empty()) {
    config.folder = DefaultSaveFolder(profile);
  }
  return config;
}

base::FilePath DefaultSaveFolder(Profile* profile) {
  return DownloadPrefs::FromBrowserContext(profile)->DownloadPath();
}

bool ClaudeCodeOwnToolsOn(Profile* profile) {
  const PrefService* prefs = profile->GetPrefs();
  if (prefs->GetInteger(prefs::kBackend) !=
          static_cast<int>(Backend::kClaudeCode) ||
      !prefs->GetBoolean(prefs::kClaudeCodeOwnTools)) {
    return false;
  }
  const FilesConfig config = GetFilesConfig(profile);
  return config.access == prefs::FilesAccess::kFolder &&
         !config.folder.empty();
}

}  // namespace compass::claude
