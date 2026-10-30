// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <AppKit/AppKit.h>

#include "compass/browser/ui/views/frame/window_frame.h"

#include "ui/gfx/native_ui_types.h"
#include "ui/views/widget/widget.h"

namespace compass {

void SetCaptionButtonsOpacity(views::Widget* widget, float opacity) {
  // Null for a window in another process (an app shim's), which browser
  // windows never are.
  NSWindow* window = widget->GetNativeWindow().GetNativeNSWindow();
  for (NSWindowButton type :
       {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
    NSButton* button = [window standardWindowButton:type];
    button.alphaValue = opacity;
    button.hidden = opacity == 0;
  }
}

}  // namespace compass
