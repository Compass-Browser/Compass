// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <AppKit/AppKit.h>

#include "strata/browser/ui/views/frame/window_frame.h"

#include "ui/gfx/native_ui_types.h"
#include "ui/views/widget/widget.h"

namespace strata {

void SetCaptionButtonsHidden(views::Widget* widget, bool hidden) {
  // Null for a window in another process (an app shim's), which browser
  // windows never are.
  NSWindow* window = widget->GetNativeWindow().GetNativeNSWindow();
  for (NSWindowButton type :
       {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
    [window standardWindowButton:type].hidden = hidden;
  }
}

}  // namespace strata
