// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_MAC_H_
#define COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_MAC_H_

#include <optional>

#include "ui/gfx/geometry/rect.h"

namespace views {
class Widget;
}  // namespace views

namespace compass {

// The window a link came from: the frontmost on-screen window of another app
// (the one that asked macOS to open the link). Bounds in screen coordinates.
struct OpenerWindow {
  gfx::Rect bounds;
  int number = 0;
};

// Finds it, or nullopt (no window of another app, or not on macOS).
std::optional<OpenerWindow> FindOpenerWindow();

// Puts Compass's other windows that are above the opener's window back
// under it, keeping their order, so that only |little| comes over the app
// the link came from: macOS raises every window of an app it activates. Done
// now and again as activation settles.
void KeepOtherWindowsBehind(views::Widget* little, const OpenerWindow& opener);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_MAC_H_
