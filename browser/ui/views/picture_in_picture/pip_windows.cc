// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/picture_in_picture/pip_windows.h"

#include <algorithm>

#include "base/no_destructor.h"
#include "base/time/time.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/geometry/size_conversions.h"

namespace compass {

namespace {

// How often the pointer is checked while click-through is on: the window
// ignores the mouse, so it gets no mouse events of its own.
constexpr base::TimeDelta kHoverInterval = base::Milliseconds(50);

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
  const std::vector<ui::Accelerator> accelerators =
      GetPipClickThroughAccelerators();
  return accelerators.empty() ? ui::Accelerator() : accelerators.front();
}

// static
PipWindows& PipWindows::Get() {
  static base::NoDestructor<PipWindows> instance;
  return *instance;
}

PipWindows::PipWindows() {
  settings_observation_.Observe(&ShortcutSettings::Get());
}
PipWindows::~PipWindows() = default;

void PipWindows::AddWindow(views::Widget* widget, Kind kind) {
  const bool added = !std::ranges::contains(windows_, widget);
  if (added) {
    windows_.push_back(widget);
    observations_.AddObservation(widget);
    if (kind == Kind::kDocument) {
      documents_.push_back(widget);
    }
  }
  PlatformAddWindow(widget);
  PlatformSetIgnoresMouse(widget, enabled_);
  UpdateListening();
  PlatformUpdateEventMonitor();
}

void PipWindows::Toggle() {
  if (windows_.empty()) {
    return;
  }
  enabled_ = !enabled_;
  for (views::Widget* widget : windows_) {
    PlatformSetIgnoresMouse(widget, enabled_);
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

bool PipWindows::IsClickThrough(const views::Widget* widget) const {
  return enabled_ && std::ranges::contains(windows_, widget);
}

bool PipWindows::TakesGestures(const views::Widget* widget) const {
  return std::ranges::contains(windows_, widget) &&
         !std::ranges::contains(documents_, widget);
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
  std::erase(documents_, widget);
  std::erase(hovered_, widget);
  PlatformRemoveWindow(widget);
  // Click-through ends with the last window: the next one takes clicks.
  if (windows_.empty()) {
    enabled_ = false;
    hover_timer_.Stop();
  }
  UpdateListening();
  PlatformUpdateEventMonitor();
}

void PipWindows::OnKeyPressed(const ui::Accelerator& accelerator) {
  if (accelerator.key_code() != ui::VKEY_UNKNOWN &&
      accelerator == GetAccelerator()) {
    Toggle();
  }
}

void PipWindows::OnShortcutsChanged() {
  UpdateListening();
}

void PipWindows::OnRecordingChanged() {
  UpdateListening();
}

void PipWindows::UpdateHover() {
  const gfx::Point pointer =
      display::Screen::Get()->GetCursorScreenPoint();
  for (views::Widget* widget : windows_) {
    SetHovered(widget, enabled_ && widget->IsVisible() &&
                           widget->GetWindowBoundsInScreen().Contains(pointer));
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
  PlatformFade(widget, hovered ? kHoverOpacity : 1.0f);
}

void PipWindows::UpdateListening() {
  ui::GlobalAcceleratorListener* listener =
      ui::GlobalAcceleratorListener::GetInstance();
  if (!listener) {
    return;
  }
  const ui::Accelerator accelerator = GetAccelerator();
  bool usable = accelerator.key_code() != ui::VKEY_UNKNOWN;
#if BUILDFLAG(IS_WIN)
  // Windows' hot keys here take Shift, Ctrl and Alt only: a shortcut with
  // the Windows key would be registered without it.
  usable = usable && !accelerator.IsCmdDown();
#endif
  // Not while the settings page records keys: the hot key would take them.
  const bool listen =
      !windows_.empty() && !ShortcutSettings::Get().recording() && usable;
  if (listening_ && (!listen || accelerator != listening_accelerator_)) {
    listener->UnregisterAccelerator(listening_accelerator_, this);
    listening_ = false;
  }
  if (listen && !listening_) {
    listening_ = listener->RegisterAccelerator(accelerator, this);
    listening_accelerator_ = accelerator;
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
  PlatformAnimateBounds(widget, snapped);
}

}  // namespace compass
