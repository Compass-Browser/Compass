// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/picture_in_picture/pip_click_through_mac.h"

#import <AppKit/AppKit.h>

#include <algorithm>

#include "base/no_destructor.h"
#include "base/time/time.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
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

}  // namespace

// static
ui::Accelerator PipClickThrough::GetAccelerator() {
  return ui::Accelerator(
      ui::VKEY_P, ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN | ui::EF_COMMAND_DOWN);
}

// static
PipClickThrough& PipClickThrough::Get() {
  static base::NoDestructor<PipClickThrough> instance;
  return *instance;
}

PipClickThrough::PipClickThrough() = default;
PipClickThrough::~PipClickThrough() = default;

void PipClickThrough::AddWindow(views::Widget* widget) {
  if (!std::ranges::contains(windows_, widget)) {
    windows_.push_back(widget);
    observations_.AddObservation(widget);
  }
  Apply(widget);
  UpdateListening();
}

void PipClickThrough::Toggle() {
  if (windows_.empty()) {
    return;
  }
  enabled_ = !enabled_;
  for (views::Widget* widget : windows_) {
    Apply(widget);
  }
  if (enabled_) {
    hover_timer_.Start(FROM_HERE, kHoverInterval, this,
                       &PipClickThrough::UpdateHover);
    UpdateHover();
  } else {
    hover_timer_.Stop();
    for (views::Widget* widget : std::vector(hovered_)) {
      SetHovered(widget, false);
    }
  }
}

bool PipClickThrough::IsIgnoringMouseForTesting(views::Widget* widget) const {
  return GetNSWindow(widget).ignoresMouseEvents;
}

bool PipClickThrough::IsHoveredForTesting(views::Widget* widget) const {
  return std::ranges::contains(hovered_, widget);
}

void PipClickThrough::OnWidgetDestroying(views::Widget* widget) {
  RemoveWindow(widget);
}

void PipClickThrough::OnWidgetVisibilityChanged(views::Widget* widget,
                                                bool visible) {
  if (!visible) {
    RemoveWindow(widget);
  }
}

void PipClickThrough::RemoveWindow(views::Widget* widget) {
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
}

void PipClickThrough::OnKeyPressed(const ui::Accelerator& accelerator) {
  if (accelerator == GetAccelerator()) {
    Toggle();
  }
}

void PipClickThrough::Apply(views::Widget* widget) {
  GetNSWindow(widget).ignoresMouseEvents = enabled_;
}

void PipClickThrough::UpdateHover() {
  const NSPoint pointer = NSEvent.mouseLocation;
  for (views::Widget* widget : windows_) {
    NSWindow* window = GetNSWindow(widget);
    SetHovered(widget, enabled_ && window.visible &&
                           NSPointInRect(pointer, window.frame));
  }
}

void PipClickThrough::SetHovered(views::Widget* widget, bool hovered) {
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

void PipClickThrough::UpdateListening() {
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

}  // namespace compass
