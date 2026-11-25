// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BROWSER_IMPORT_DETECT_H_
#define COMPASS_BROWSER_BROWSER_IMPORT_DETECT_H_

#include <optional>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "build/build_config.h"
#include "compass/browser/browser_import/source.h"

// Finding other browsers' profiles on this computer
// (docs/design/browser-import.md, "Where browsers keep their profiles").
namespace compass::browser_import {

// Where a browser may keep its profiles: Chromium's User Data, the folder
// of Firefox's profiles.ini, Safari's ~/Library/Safari.
struct BrowserRoot {
  Browser browser;
  base::FilePath root;
};

// This platform's places to look, in the page's order. A browser may have
// several (Linux packages); the first one found counts.
std::vector<BrowserRoot> CandidateRoots();

// Tests: look only at |roots| (nullopt: CandidateRoots()).
void SetRootsForTesting(std::optional<std::vector<BrowserRoot>> roots);

// The profiles of the browsers found, browser by browser in the page's
// order, each browser's in its own order. Blocking.
std::vector<SourceProfile> DetectProfiles();

// The major Chromium version that last opened the Chromium-based browser's
// |user_data| (its "Last Version" file, "157.0.8103.51" → 157), or nullopt
// if there's none. Chrome, Edge, Brave, Vivaldi and Dia write Chromium's
// version there, not their own. A profile copied from a newer version may
// have databases Compass can't open (docs/design/browser-import.md, "As a
// profile"). Blocking.
std::optional<int> ChromiumMajorVersion(const base::FilePath& user_data);

// Whether |profile|'s browser is running (it then has its files open: the
// page asks to quit it first). Blocking.
bool IsRunning(const SourceProfile& profile);

#if BUILDFLAG(IS_MAC)
// Whether an app with |bundle_id| is running.
bool IsMacAppRunning(std::string_view bundle_id);
#endif

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_BROWSER_IMPORT_DETECT_H_
