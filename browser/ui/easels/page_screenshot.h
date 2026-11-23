// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_EASELS_PAGE_SCREENSHOT_H_
#define COMPASS_BROWSER_UI_EASELS_PAGE_SCREENSHOT_H_

#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/time/time.h"
#include "base/types/expected.h"

class SkBitmap;

namespace content {
class WebContents;
}  // namespace content

// Capture Screenshot's ends (docs/design/screenshots.md): what's done with a
// region the capture overlay picked, or with the whole page.
namespace compass::easels {

enum class ScreenshotAction { kCopy, kSave };

// What the user sees afterwards: "Copied", or where it was saved; or why it
// failed.
using ScreenshotCallback =
    base::OnceCallback<void(base::expected<std::string, std::string>)>;

// Copies |bitmap| to the clipboard, or saves it as a PNG in |contents|'
// profile's download folder.
void FinishScreenshot(content::WebContents* contents,
                      ScreenshotAction action,
                      SkBitmap bitmap,
                      ScreenshotCallback done);

// The whole page, beyond the viewport (at most kMaxFullPageHeight CSS pixels
// tall), through an internal DevTools session; then as FinishScreenshot().
inline constexpr int kMaxFullPageHeight = 16384;
void CaptureFullPage(content::WebContents* contents,
                     ScreenshotAction action,
                     ScreenshotCallback done);

// "Screenshot 2026-11-23 at 14.05.09.png", in local time.
base::FilePath ScreenshotFileName(base::Time time);

}  // namespace compass::easels

#endif  // COMPASS_BROWSER_UI_EASELS_PAGE_SCREENSHOT_H_
