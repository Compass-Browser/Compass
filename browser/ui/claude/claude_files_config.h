// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_CLAUDE_FILES_CONFIG_H_
#define COMPASS_BROWSER_UI_CLAUDE_CLAUDE_FILES_CONFIG_H_

#include "base/files/file_path.h"
#include "compass/browser/claude/claude_tools.h"

class Profile;

namespace compass::claude {

// The profile's Files setting now (docs/design/claude.md, "Files"): "Save
// files" without a folder saves into Downloads.
FilesConfig GetFilesConfig(Profile* profile);

// Where "Save files" goes without a folder chosen: Downloads.
base::FilePath DefaultSaveFolder(Profile* profile);

// Claude Code mode runs with its own tools: Claude Code is the backend,
// "Use a folder" has a folder, and the switch is on.
bool ClaudeCodeOwnToolsOn(Profile* profile);

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_CLAUDE_CLAUDE_FILES_CONFIG_H_
