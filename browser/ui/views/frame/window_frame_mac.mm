// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <AppKit/AppKit.h>

#include "compass/browser/ui/views/frame/window_frame.h"

#import "components/remote_cocoa/app_shim/native_widget_mac_nswindow.h"
#include "ui/color/color_provider_key.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/views/cocoa/native_widget_mac_ns_window_host.h"
#include "ui/views/widget/widget.h"

// The window's backdrop: macOS's sidebar material, behind the compositor's
// view (the bridge sorts NSVisualEffectViews first). It lets clicks through
// to the views over it.
@interface CompassBackdropView : NSVisualEffectView
@end

@implementation CompassBackdropView
- (NSView*)hitTest:(NSPoint)point {
  return nil;
}
@end

namespace compass {

namespace {

CompassBackdropView* FindBackdrop(NSView* content_view) {
  for (NSView* view in content_view.subviews) {
    if ([view isKindOfClass:[CompassBackdropView class]]) {
      return static_cast<CompassBackdropView*>(view);
    }
  }
  return nil;
}

}  // namespace

bool IsWindowBackdropSupported() {
  return true;
}

void SetWindowBackdrop(views::Widget* widget, bool on) {
  views::NativeWidgetMacNSWindowHost* host =
      views::NativeWidgetMacNSWindowHost::GetFromNativeWindow(
          widget->GetNativeWindow());
  // Browser windows are always in this process.
  NSWindow* window = host ? host->GetInProcessNSWindow() : nil;
  if (!window) {
    return;
  }
  NSView* content_view = window.contentView;
  CompassBackdropView* backdrop = FindBackdrop(content_view);
  if (!on && !backdrop) {
    return;
  }
  if (on && !backdrop) {
    host->SetLayerAndCompositorOpaque(false);
    window.opaque = NO;
    // As upstream's glass frame: a clear background has AppKit invalidate
    // the window's surface all the time; this is as good as clear.
    window.backgroundColor =
        [NSColor.windowBackgroundColor colorWithAlphaComponent:0.001];
    backdrop = [[CompassBackdropView alloc] initWithFrame:content_view.bounds];
    backdrop.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    backdrop.material = NSVisualEffectMaterialSidebar;
    backdrop.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    backdrop.state = NSVisualEffectStateFollowsWindowActiveState;
    [content_view addSubview:backdrop positioned:NSWindowBelow relativeTo:nil];
  }
  if (on) {
    // The window's mode, which a space can set apart from the system's: the
    // window's own appearance follows the system.
    backdrop.appearance = [NSAppearance
        appearanceNamed:widget->GetColorMode() ==
                                ui::ColorProviderKey::ColorMode::kDark
                            ? NSAppearanceNameDarkAqua
                            : NSAppearanceNameAqua];
  } else {
    [backdrop removeFromSuperview];
    host->SetLayerAndCompositorOpaque(true);
    window.backgroundColor = NSColor.windowBackgroundColor;
    window.opaque = YES;
  }
}

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
