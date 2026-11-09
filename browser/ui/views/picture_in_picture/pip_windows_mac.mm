// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// PipWindows' macOS part (pip_windows.h).

#include "compass/browser/ui/views/picture_in_picture/pip_windows.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <cmath>

#include "base/time/time.h"
#include "ui/gfx/mac/coordinate_conversion.h"
#include "ui/gfx/native_ui_types.h"

namespace compass {

namespace {

constexpr NSTimeInterval kFadeDuration = 0.15;

NSWindow* GetNSWindow(views::Widget* widget) {
  return widget->GetNativeWindow().GetNativeNSWindow();
}

// The gesture monitor, while a window is open.
id __strong g_event_monitor = nil;

// The app in front when a window was last clicked while Compass wasn't
// active, and when; nil after a click with Compass active, or once used.
NSRunningApplication* __strong g_front_app = nil;
base::TimeTicks g_front_app_click_time;
// How long after the click a window closing counts as closed by it, and how
// long after it closes macOS may still activate Compass.
constexpr base::TimeDelta kClickToCloseTime = base::Seconds(2);
constexpr base::TimeDelta kActivationGrace = base::Seconds(1);

views::Widget* FindWindow(
    const std::vector<raw_ptr<views::Widget>>& windows,
    NSEvent* event) {
  if (!event.window) {
    return nullptr;
  }
  for (views::Widget* widget : windows) {
    if (GetNSWindow(widget) == event.window) {
      return widget;
    }
  }
  return nullptr;
}

bool HasEnded(NSEventPhase phase) {
  return phase & (NSEventPhaseEnded | NSEventPhaseCancelled);
}

// A window closed after a click on it while another app was in front
// (close, minimize): macOS then gives Compass the focus and brings its
// windows up. That app comes back to the front.
void RestoreFrontApp() {
  NSRunningApplication* front_app = g_front_app;
  g_front_app = nil;
  if (!front_app || front_app.terminated ||
      base::TimeTicks::Now() - g_front_app_click_time > kClickToCloseTime) {
    return;
  }
  // Now, or when macOS gets to it.
  if (NSApp.active) {
    [front_app activateWithOptions:0];
    return;
  }
  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  __block id observer = [center
      addObserverForName:NSApplicationDidBecomeActiveNotification
                  object:nil
                   queue:nil
              usingBlock:^(NSNotification*) {
                [front_app activateWithOptions:0];
                [NSNotificationCenter.defaultCenter removeObserver:observer];
                observer = nil;
              }];
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW, kActivationGrace.InNanoseconds()),
      dispatch_get_main_queue(), ^{
        if (observer) {
          [NSNotificationCenter.defaultCenter removeObserver:observer];
          observer = nil;
        }
      });
}

// Rounds the window's corners, without the system shadow.
void RoundCorners(NSWindow* window) {
  NSView* content = window.contentView;
  content.wantsLayer = YES;
  content.layer.cornerRadius = PipWindows::kCornerRadius;
  content.layer.cornerCurve = kCACornerCurveContinuous;
  content.layer.masksToBounds = YES;
  // The frame view around it draws a light line along the window's top
  // edge, square to its corners: round and clip it too, and hide its title
  // bar views (the window has no title bar).
  if (NSView* frame_view = content.superview) {
    frame_view.wantsLayer = YES;
    frame_view.layer.cornerRadius = PipWindows::kCornerRadius;
    frame_view.layer.cornerCurve = kCACornerCurveContinuous;
    frame_view.layer.masksToBounds = YES;
    for (NSView* view in frame_view.subviews) {
      if ([NSStringFromClass(view.class) hasPrefix:@"NSTitlebar"]) {
        view.hidden = YES;
      }
    }
  }
  // No system shadow: it comes with a dark outline around the window.
  window.hasShadow = NO;
}

}  // namespace

void PipWindows::PlatformAddWindow(views::Widget* widget) {
  NSWindow* window = GetNSWindow(widget);
  RoundCorners(window);
  // No macOS window tiling: Compass snaps it to the edges itself.
  window.collectionBehavior =
      (window.collectionBehavior &
       ~NSWindowCollectionBehaviorFullScreenAllowsTiling) |
      NSWindowCollectionBehaviorFullScreenDisallowsTiling;
}

void PipWindows::PlatformRemoveWindow(views::Widget* widget) {
  NSWindow* window = GetNSWindow(widget);
  window.ignoresMouseEvents = NO;
  window.alphaValue = 1.0;
  RestoreFrontApp();
}

void PipWindows::PlatformSetIgnoresMouse(views::Widget* widget,
                                         bool ignores) {
  GetNSWindow(widget).ignoresMouseEvents = ignores;
}

void PipWindows::PlatformFade(views::Widget* widget, float opacity) {
  NSWindow* window = GetNSWindow(widget);
  [NSAnimationContext
      runAnimationGroup:^(NSAnimationContext* context) {
        context.duration = kFadeDuration;
        window.animator.alphaValue = opacity;
      }
      completionHandler:nil];
}

void PipWindows::PlatformAnimateBounds(views::Widget* widget,
                                       const gfx::Rect& bounds) {
  [GetNSWindow(widget) setFrame:gfx::ScreenRectToNSRect(bounds)
                        display:YES
                        animate:YES];
}

bool PipWindows::IsIgnoringMouseForTesting(views::Widget* widget) const {
  return GetNSWindow(widget).ignoresMouseEvents;
}

bool PipWindows::HasRoundCornersForTesting(views::Widget* widget) const {
  NSView* content = GetNSWindow(widget).contentView;
  return content.layer && content.layer.cornerRadius == kCornerRadius;
}

bool PipWindows::AllowsTilingForTesting(views::Widget* widget) const {
  return !(GetNSWindow(widget).collectionBehavior &
           NSWindowCollectionBehaviorFullScreenDisallowsTiling);
}

void PipWindows::WillFocusOpener() {
  g_front_app = nil;
}

void PipWindows::PlatformUpdateEventMonitor() {
  const bool monitor = !windows_.empty();
  if (monitor == !!g_event_monitor) {
    return;
  }
  if (!monitor) {
    [NSEvent removeMonitor:g_event_monitor];
    g_event_monitor = nil;
    return;
  }
  PipWindows* self = this;  // A singleton: outlives the monitor.
  g_event_monitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify |
                                           NSEventMaskScrollWheel |
                                           NSEventMaskLeftMouseDown |
                                           NSEventMaskLeftMouseUp
                                   handler:^NSEvent*(NSEvent* event) {
                                     views::Widget* widget =
                                         FindWindow(self->windows_, event);
                                     if (!widget ||
                                         self->IsClickThrough(widget)) {
                                       return event;
                                     }
                                     // A document window's page keeps
                                     // its pinches and scrolls.
                                     if (!self->TakesGestures(widget) &&
                                         (event.type == NSEventTypeMagnify ||
                                          event.type ==
                                              NSEventTypeScrollWheel)) {
                                       return event;
                                     }
                                     switch (event.type) {
                                       case NSEventTypeMagnify:
                                         self->ZoomWindow(
                                             widget, event.magnification);
                                         if (HasEnded(event.phase)) {
                                           self->EndGesture(widget);
                                         }
                                         return nil;
                                       case NSEventTypeScrollWheel: {
                                         // A mouse wheel goes on as before.
                                         if (!event.hasPreciseScrollingDeltas) {
                                           return event;
                                         }
                                         // Momentum after the fingers lift
                                         // doesn't move it: it stops, and
                                         // snaps, where they left it.
                                         if (event.momentumPhase !=
                                             NSEventPhaseNone) {
                                           return nil;
                                         }
                                         // With the fingers, whichever way
                                         // scrolling goes.
                                         const double sign =
                                             event.directionInvertedFromDevice
                                                 ? 1
                                                 : -1;
                                         self->MoveWindow(
                                             widget,
                                             gfx::Vector2d(
                                                 std::round(
                                                     sign *
                                                     event.scrollingDeltaX),
                                                 std::round(
                                                     sign *
                                                     event.scrollingDeltaY)));
                                         if (HasEnded(event.phase)) {
                                           self->EndGesture(widget);
                                         }
                                         return nil;
                                       }
                                       case NSEventTypeLeftMouseDown:
                                         // Whose the front was, in case
                                         // this click closes the window.
                                         g_front_app =
                                             NSApp.active
                                                 ? nil
                                                 : NSWorkspace.sharedWorkspace
                                                       .frontmostApplication;
                                         g_front_app_click_time =
                                             base::TimeTicks::Now();
                                         return event;
                                       case NSEventTypeLeftMouseUp:
                                         // After a drag of the window.
                                         self->EndGesture(widget);
                                         return event;
                                       default:
                                         return event;
                                     }
                                   }];
}

}  // namespace compass
