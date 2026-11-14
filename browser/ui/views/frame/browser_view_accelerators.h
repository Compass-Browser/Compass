// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_FRAME_BROWSER_VIEW_ACCELERATORS_H_
#define COMPASS_BROWSER_UI_VIEWS_FRAME_BROWSER_VIEW_ACCELERATORS_H_

class BrowserView;

namespace compass {

// Windows and Linux: every browser window's shortcuts follow the user's
// (docs/design/shortcuts.md, "Windows and Linux"). A window registers
// upstream's accelerator table, which compass::AdjustAcceleratorList()
// adjusted, once (BrowserView::LoadAccelerators()); when the user changes a
// shortcut, each window registers the table again. While the settings page records keys,
// the windows register none, so every key reaches the page.
class BrowserViewAccelerators {
 public:
  // Starts following the user's shortcuts, once. The settings page calls
  // it: shortcuts change, and recording happens, only there.
  static void Watch();

  // |view| registers the table again, or none.
  static void Reload(BrowserView& view);
  static void Drop(BrowserView& view);
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_FRAME_BROWSER_VIEW_ACCELERATORS_H_
