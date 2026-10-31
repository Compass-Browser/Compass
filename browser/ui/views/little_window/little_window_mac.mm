// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/little_window/little_window_mac.h"

#import <AppKit/AppKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <unistd.h>

#include <vector>

#include "base/apple/bridging.h"
#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/views/widget/widget.h"

namespace compass {

namespace {

// After showing the window: once more when the activation macOS started has
// raised the app's windows, and once in case it came later still.
constexpr base::TimeDelta kSettleDelay = base::Milliseconds(300);
constexpr base::TimeDelta kActivationWatch = base::Seconds(2);

NSArray* CopyOnScreenWindows() {
  return base::apple::CFToNSOwnershipCast(CGWindowListCopyWindowInfo(
      kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
      kCGNullWindowID));
}

void ReorderBehind(NSWindow* little, NSInteger opener) {
  if (!little) {
    return;
  }
  // Front to back: ours above the opener's window, in that order.
  std::vector<NSInteger> above;
  const pid_t self = getpid();
  bool found = false;
  for (NSDictionary* info in CopyOnScreenWindows()) {
    const NSInteger number = [info[(id)kCGWindowNumber] integerValue];
    if (number == opener) {
      found = true;
      break;
    }
    if ([info[(id)kCGWindowOwnerPID] intValue] == self &&
        [info[(id)kCGWindowLayer] intValue] == 0) {
      above.push_back(number);
    }
  }
  if (!found) {
    return;
  }
  // Back to front, each just under the opener's window: the order holds.
  for (auto it = above.rbegin(); it != above.rend(); ++it) {
    NSWindow* window = [NSApp windowWithWindowNumber:*it];
    if (!window || window == little || window.parentWindow ||
        window.level != NSNormalWindowLevel) {
      continue;
    }
    [window orderWindow:NSWindowBelow relativeTo:opener];
  }
  [little orderFront:nil];
}

}  // namespace

std::optional<OpenerWindow> FindOpenerWindow() {
  const pid_t self = getpid();
  for (NSDictionary* info in CopyOnScreenWindows()) {
    if ([info[(id)kCGWindowLayer] intValue] != 0 ||
        [info[(id)kCGWindowOwnerPID] intValue] == self ||
        [info[(id)kCGWindowAlpha] doubleValue] <= 0) {
      continue;
    }
    CGRect rect;
    if (!CGRectMakeWithDictionaryRepresentation(
            (__bridge CFDictionaryRef)info[(id)kCGWindowBounds], &rect) ||
        rect.size.width < 100 || rect.size.height < 100) {
      continue;
    }
    // CoreGraphics' global coordinates, like gfx's screen coordinates on
    // macOS, start at the primary display's top left.
    return OpenerWindow{gfx::Rect(rect),
                        static_cast<int>(
                            [info[(id)kCGWindowNumber] integerValue])};
  }
  return std::nullopt;
}

void KeepOtherWindowsBehind(views::Widget* little, const OpenerWindow& opener) {
  NSWindow* window = little->GetNativeWindow().GetNativeNSWindow();
  const NSInteger number = opener.number;
  ReorderBehind(window, number);

  __weak NSWindow* weak_window = window;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                               kSettleDelay.InNanoseconds()),
                 dispatch_get_main_queue(), ^{
                   ReorderBehind(weak_window, number);
                 });

  __block id observer = [NSNotificationCenter.defaultCenter
      addObserverForName:NSApplicationDidBecomeActiveNotification
                  object:NSApp
                   queue:nil
              usingBlock:^(NSNotification*) {
                ReorderBehind(weak_window, number);
              }];
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW, kActivationWatch.InNanoseconds()),
      dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter removeObserver:observer];
      });
}

}  // namespace compass
