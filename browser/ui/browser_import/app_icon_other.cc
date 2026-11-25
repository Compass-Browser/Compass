// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/browser_import/app_icon.h"

namespace compass::browser_import {

// Linux: the icon theme would say (the .desktop file's Icon=), not yet read;
// the page shows letters.
std::string BrowserAppIcon(const SourceProfile& profile) {
  return std::string();
}

std::string ArcAppIcon() {
  return std::string();
}

}  // namespace compass::browser_import
