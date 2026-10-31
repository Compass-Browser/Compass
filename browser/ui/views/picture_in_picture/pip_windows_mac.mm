// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/picture_in_picture/pip_windows_mac.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>

#include "base/no_destructor.h"
#include "base/time/time.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/mac/coordinate_conversion.h"
#include "ui/gfx/native_ui_types.h"

namespace compass {

namespace {

// How often the pointer is checked while click-through is on: the window
// ignores the mouse, so it gets no mouse events of its own.
constexpr base::TimeDelta kHoverInterval = base::Milliseconds(50);
constexpr NSTimeInterval kFadeDuration = 0.15;

NSWindow* GetNSWindow(views::Widget* widget) {
  return widget->GetNativeWindow().GetNativeNSWindow();
}

// The gesture monitor, while a window is open.
id __strong g_event_monitor = nil;

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

}  // namespace

// static
gfx::Rect PipWindows::Zoom(const gfx::Rect& bounds,
                           double magnification,
                           const gfx::Size& min_size,
                           const gfx::Size& max_size) {
  if (bounds.IsEmpty()) {
    return bounds;
  }
  double scale = std::max(0.0, 1.0 + magnification);
  const double width = bounds.width();
  const double height = bounds.height();
  // Within the sizes, both sides by the same scale.
  if (!min_size.IsEmpty()) {
    scale = std::max({scale, min_size.width() / width,
                      min_size.height() / height});
  }
  if (!max_size.IsEmpty()) {
    scale = std::min({scale, max_size.width() / width,
                      max_size.height() / height});
  }
  const gfx::Size size = gfx::ToRoundedSize(
      gfx::SizeF(static_cast<float>(width * scale),
                 static_cast<float>(height * scale)));
  const gfx::Point center = bounds.CenterPoint();
  return gfx::Rect(center.x() - size.width() / 2,
                   center.y() - size.height() / 2, size.width(),
                   size.height());
}

// static
gfx::Rect PipWindows::Move(const gfx::Rect& bounds,
                           const gfx::Vector2d& delta,
                           const gfx::Rect& work_area) {
  gfx::Rect moved = bounds + delta;
  if (!work_area.IsEmpty()) {
    moved.AdjustToFit(work_area);
  }
  return moved;
}

// static
gfx::Rect PipWindows::Snap(const gfx::Rect& bounds,
                           const gfx::Rect& work_area) {
  if (work_area.IsEmpty()) {
    return bounds;
  }
  gfx::Rect snapped = bounds;
  snapped.AdjustToFit(work_area);
  if (snapped.x() - work_area.x() < kSnapDistance) {
    snapped.set_x(work_area.x() + kEdgeMargin);
  } else if (work_area.right() - snapped.right() < kSnapDistance) {
    snapped.set_x(work_area.right() - kEdgeMargin - snapped.width());
  }
  if (snapped.y() - work_area.y() < kSnapDistance) {
    snapped.set_y(work_area.y() + kEdgeMargin);
  } else if (work_area.bottom() - snapped.bottom() < kSnapDistance) {
    snapped.set_y(work_area.bottom() - kEdgeMargin - snapped.height());
  }
  return snapped;
}

// static
ui::Accelerator PipWindows::GetAccelerator() {
  return ui::Accelerator(
      ui::VKEY_P, ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN | ui::EF_COMMAND_DOWN);
}

// static
PipWindows& PipWindows::Get() {
  static base::NoDestructor<PipWindows> instance;
  return *instance;
}

PipWindows::PipWindows() = default;
PipWindows::~PipWindows() = default;

void PipWindows::AddWindow(views::Widget* widget) {
  if (!std::ranges::contains(windows_, widget)) {
    windows_.push_back(widget);
    observations_.AddObservation(widget);
  }
  RoundCorners(widget);
  Apply(widget);
  UpdateListening();
  UpdateEventMonitor();
}

void PipWindows::Toggle() {
  if (windows_.empty()) {
    return;
  }
  enabled_ = !enabled_;
  for (views::Widget* widget : windows_) {
    Apply(widget);
  }
  if (enabled_) {
    hover_timer_.Start(FROM_HERE, kHoverInterval, this,
                       &PipWindows::UpdateHover);
    UpdateHover();
  } else {
    hover_timer_.Stop();
    for (views::Widget* widget : std::vector(hovered_)) {
      SetHovered(widget, false);
    }
  }
}

bool PipWindows::IsIgnoringMouseForTesting(views::Widget* widget) const {
  return GetNSWindow(widget).ignoresMouseEvents;
}

bool PipWindows::IsClickThrough(const views::Widget* widget) const {
  return enabled_ && std::ranges::contains(windows_, widget);
}

double PipWindows::GetCornerRadiusForTesting(
    views::Widget* widget) const {
  NSView* content = GetNSWindow(widget).contentView;
  return content.layer ? content.layer.cornerRadius : 0;
}

void PipWindows::RoundCorners(views::Widget* widget) {
  NSWindow* window = GetNSWindow(widget);
  NSView* content = window.contentView;
  content.wantsLayer = YES;
  content.layer.cornerRadius = kCornerRadius;
  content.layer.cornerCurve = kCACornerCurveContinuous;
  content.layer.masksToBounds = YES;
  // The shadow is drawn from what the window paints: redraw it rounded.
  [window invalidateShadow];
}

bool PipWindows::IsHoveredForTesting(views::Widget* widget) const {
  return std::ranges::contains(hovered_, widget);
}

void PipWindows::OnWidgetDestroying(views::Widget* widget) {
  RemoveWindow(widget);
}

void PipWindows::OnWidgetVisibilityChanged(views::Widget* widget,
                                                bool visible) {
  if (!visible) {
    RemoveWindow(widget);
  }
}

void PipWindows::RemoveWindow(views::Widget* widget) {
  if (!std::ranges::contains(windows_, widget)) {
    return;
  }
  observations_.RemoveObservation(widget);
  std::erase(windows_, widget);
  std::erase(hovered_, widget);
  NSWindow* window = GetNSWindow(widget);
  window.ignoresMouseEvents = NO;
  window.alphaValue = 1.0;
  // Click-through ends with the last window: the next one takes clicks.
  if (windows_.empty()) {
    enabled_ = false;
    hover_timer_.Stop();
  }
  UpdateListening();
  UpdateEventMonitor();
}

void PipWindows::OnKeyPressed(const ui::Accelerator& accelerator) {
  if (accelerator == GetAccelerator()) {
    Toggle();
  }
}

void PipWindows::Apply(views::Widget* widget) {
  GetNSWindow(widget).ignoresMouseEvents = enabled_;
}

void PipWindows::UpdateHover() {
  const NSPoint pointer = NSEvent.mouseLocation;
  for (views::Widget* widget : windows_) {
    NSWindow* window = GetNSWindow(widget);
    SetHovered(widget, enabled_ && window.visible &&
                           NSPointInRect(pointer, window.frame));
  }
}

void PipWindows::SetHovered(views::Widget* widget, bool hovered) {
  if (std::ranges::contains(hovered_, widget) == hovered) {
    return;
  }
  if (hovered) {
    hovered_.push_back(widget);
  } else {
    std::erase(hovered_, widget);
  }
  NSWindow* window = GetNSWindow(widget);
  [NSAnimationContext
      runAnimationGroup:^(NSAnimationContext* context) {
        context.duration = kFadeDuration;
        window.animator.alphaValue = hovered ? kHoverOpacity : 1.0;
      }
      completionHandler:nil];
}

void PipWindows::UpdateListening() {
  ui::GlobalAcceleratorListener* listener =
      ui::GlobalAcceleratorListener::GetInstance();
  const bool listen = !windows_.empty();
  if (!listener || listen == listening_) {
    return;
  }
  if (listen) {
    listening_ = listener->RegisterAccelerator(GetAccelerator(), this);
  } else {
    listener->UnregisterAccelerator(GetAccelerator(), this);
    listening_ = false;
  }
}

gfx::Rect PipWindows::GetWorkArea(views::Widget* widget) const {
  return display::Screen::Get()
      ->GetDisplayMatching(widget->GetWindowBoundsInScreen())
      .work_area();
}

void PipWindows::ZoomWindow(views::Widget* widget, double magnification) {
  const gfx::Rect work_area = GetWorkArea(widget);
  gfx::Size max_size = widget->GetMaximumSize();
  if (max_size.IsEmpty()) {
    max_size = work_area.size();
  }
  gfx::Rect bounds = Zoom(widget->GetWindowBoundsInScreen(), magnification,
                          widget->GetMinimumSize(), max_size);
  // Growing near an edge pushes it back on screen.
  bounds.AdjustToFit(work_area);
  widget->SetBounds(bounds);
}

void PipWindows::MoveWindow(views::Widget* widget,
                            const gfx::Vector2d& delta) {
  widget->SetBounds(
      Move(widget->GetWindowBoundsInScreen(), delta, GetWorkArea(widget)));
}

void PipWindows::EndGesture(views::Widget* widget, bool animate) {
  const gfx::Rect bounds = widget->GetWindowBoundsInScreen();
  const gfx::Rect snapped = Snap(bounds, GetWorkArea(widget));
  if (snapped == bounds) {
    return;
  }
  if (!animate) {
    widget->SetBounds(snapped);
    return;
  }
  [GetNSWindow(widget) setFrame:gfx::ScreenRectToNSRect(snapped)
                        display:YES
                        animate:YES];
}

void PipWindows::UpdateEventMonitor() {
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
                                           NSEventMaskLeftMouseUp
                                   handler:^NSEvent*(NSEvent* event) {
                                     views::Widget* widget =
                                         FindWindow(self->windows_, event);
                                     if (!widget ||
                                         self->IsClickThrough(widget)) {
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
