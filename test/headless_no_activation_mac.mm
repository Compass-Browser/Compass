// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// In a headless browser test (`strata test --browser` passes --headless), the
// app is never active, so tests running in the background don't take the
// user's focus: macOS brings a newly launched app to the front, and showing a
// window activates it (NativeWidgetNSWindowBridge). Windows still show, as for
// background-only apps, and tests fake key and main status
// (ui::test::ScopedFakeNSWindowFocus, set up by InProcessBrowserTest).

#import <AppKit/AppKit.h>

@interface StrataNoAppActivation : NSObject
@end

@implementation StrataNoAppActivation

+ (void)load {
  if (![NSProcessInfo.processInfo.arguments containsObject:@"--headless"]) {
    return;
  }
  [NSNotificationCenter.defaultCenter
      addObserverForName:NSApplicationWillFinishLaunchingNotification
                  object:nil
                   queue:nil
              usingBlock:^(NSNotification*) {
                [NSApp setActivationPolicy:
                           NSApplicationActivationPolicyProhibited];
              }];
}

@end
