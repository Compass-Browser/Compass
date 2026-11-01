// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMMON_USER_DATA_DIR_MAC_H_
#define COMPASS_COMMON_USER_DATA_DIR_MAC_H_

#include <string>

namespace base {
class FilePath;
}  // namespace base

namespace compass {

// The folder in ~/Library/Application Support (|app_support|) that holds
// Compass's user data: "Compass". Until 2026-11-01 Compass used upstream's
// "Chromium"; the first launch that finds no "Compass" folder moves a
// "Chromium" folder that is Compass's (it has Compass's spaces file) there,
// with its cache folder in |caches| (~/Library/Caches), and rewrites the
// absolute paths into it saved in Local State and each profile's
// Preferences. A plain Chromium's folder is never touched. When the old
// folder can't be moved now (a running Compass holds it, or the move fails),
// returns "Chromium", so this launch keeps the user's data and a later one
// moves it. See docs/design/branding.md, "User data folder".
std::string ChooseMacProductDirName(const base::FilePath& app_support,
                                    const base::FilePath& caches);

// ChooseMacProductDirName() for this user's folders, computed once.
const char* GetMacProductDirName();

}  // namespace compass

#endif  // COMPASS_COMMON_USER_DATA_DIR_MAC_H_
