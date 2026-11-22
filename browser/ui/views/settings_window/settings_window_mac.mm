// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/settings_window/settings_window.h"

#import <AppKit/AppKit.h>

#include "components/remote_cocoa/common/native_widget_ns_window.mojom.h"
#include "ui/gfx/mac/coordinate_conversion.h"
#include "ui/views/widget/native_widget_mac.h"

namespace compass {

namespace {

// The page runs under a transparent title bar, as a browser window's does,
// with the traffic lights centered on the page's toolbar's top row.
class SettingsNativeWidgetMac : public views::NativeWidgetMac {
 public:
  explicit SettingsNativeWidgetMac(views::Widget* widget)
      : views::NativeWidgetMac(widget) {}

  // views::NativeWidgetMac:
  void GetWindowFrameTitlebarHeight(bool* override_titlebar_height,
                                    float* titlebar_height) override {
    // The toolbar's row of tabs is under the traffic lights' row.
    *override_titlebar_height = true;
    *titlebar_height = kTrafficLightsRowHeight;
  }

 protected:
  void PopulateCreateWindowParams(
      const views::Widget::InitParams& widget_params,
      remote_cocoa::mojom::CreateWindowParams* params) override {
    // The browser window class: it places the traffic lights by
    // GetWindowFrameTitlebarHeight().
    params->window_class = remote_cocoa::mojom::WindowClass::kBrowser;
    params->style_mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                         NSWindowStyleMaskMiniaturizable |
                         NSWindowStyleMaskFullSizeContentView;
    params->titlebar_appears_transparent = true;
    params->window_title_hidden = true;
    params->animation_enabled = true;
  }

 private:
  // The page's title row (settings_window/app.css, #titleRow).
  static constexpr float kTrafficLightsRowHeight = 30;
};

}  // namespace

views::NativeWidget* CreateSettingsNativeWidget(views::Widget* widget) {
  return new SettingsNativeWidgetMac(widget);
}

void AnimateSettingsWindowBounds(views::Widget* widget,
                                 const gfx::Rect& bounds) {
  NSWindow* window = widget->GetNativeWindow().GetNativeNSWindow();
  if (!window) {
    widget->SetBounds(bounds);
    return;
  }
  // Through the animator, which doesn't block: -setFrame:display:animate:
  // runs the animation in a nested run loop, which starves the browser.
  const NSRect frame = gfx::ScreenRectToNSRect(bounds);
  [NSAnimationContext runAnimationGroup:^(NSAnimationContext* context) {
    context.duration = 0.18;
    context.allowsImplicitAnimation = YES;
    [[window animator] setFrame:frame display:YES];
  }];
}

}  // namespace compass
