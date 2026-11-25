// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_BROWSER_IMPORT_APP_ICON_H_
#define COMPASS_BROWSER_UI_BROWSER_IMPORT_APP_ICON_H_

#include <string>

#include "compass/browser/browser_import/source.h"

class SkBitmap;

// The import page's browser icons (docs/design/browser-import.md, "The
// page"): each browser's own app icon, read from the app installed on this
// computer, so Compass ships no other browser's logo. The page falls back
// to a letter where there's none.
namespace compass::browser_import {

// The side, in pixels, the icons are read at: about twice what the page
// shows (34 CSS pixels), for high-density screens.
inline constexpr int kAppIconSize = 72;

// The icon of the app that keeps |profile|, as a PNG data: URL, or empty if
// its app wasn't found, or this platform doesn't say (Linux). Blocking.
std::string BrowserAppIcon(const SourceProfile& profile);

// Arc's (macOS), or empty. Blocking.
std::string ArcAppIcon();

// |bitmap| as a PNG data: URL; empty if it's empty or can't be encoded.
std::string PngDataUrl(const SkBitmap& bitmap);

}  // namespace compass::browser_import

#endif  // COMPASS_BROWSER_UI_BROWSER_IMPORT_APP_ICON_H_
