// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// PipWindows' Windows part (pip_windows.h).

#include "compass/browser/ui/views/picture_in_picture/pip_windows.h"

#include <windows.h>

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/win/registry.h"
#include "base/win/windows_version.h"
#include "ui/base/win/hwnd_subclass.h"
#include "ui/gfx/animation/tween.h"
#include "ui/views/win/hwnd_util.h"

namespace compass {

namespace {

constexpr base::TimeDelta kFadeDuration = base::Milliseconds(150);
constexpr base::TimeDelta kBoundsDuration = base::Milliseconds(200);
constexpr base::TimeDelta kFrameInterval = base::Milliseconds(16);
// Wheel messages carry no phase: a gesture has ended once none came for
// this long.
constexpr base::TimeDelta kGestureEndDelay = base::Milliseconds(150);
// Ctrl+wheel (a touchpad pinch, or Ctrl and a mouse wheel): a wheel notch
// (WHEEL_DELTA, 120) resizes by 10%.
constexpr double kWheelUnitsPerZoom = 1200;
// Two fingers on a touchpad: 120 wheel units, a notch, scroll about 100 DIP
// in most apps, so the window keeps up with the page it's over.
constexpr double kDipPerWheelUnit = 100.0 / WHEEL_DELTA;

HWND GetHwnd(views::Widget* widget) {
  return views::HWNDForWidget(widget);
}

BYTE ToAlpha(float opacity) {
  return static_cast<BYTE>(std::lround(std::clamp(opacity, 0.0f, 1.0f) * 255));
}

bool IsLayered(HWND hwnd) {
  return ::GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED;
}

// Whether two fingers moving down scroll up, as a page follows them
// ("Scrolling direction" in the touchpad settings; Windows' default).
bool TouchpadScrollsNaturally() {
  base::win::RegKey key(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad",
      KEY_QUERY_VALUE);
  DWORD direction = 0;
  if (!key.Valid() ||
      key.ReadValueDW(L"ScrollDirection", &direction) != ERROR_SUCCESS) {
    return true;
  }
  // 0: "Down motion scrolls down".
  return direction != 0;
}

// What Compass keeps for a picture-in-picture window. Its timers call back
// into it, so they stop when it goes.
struct WindowState {
  WindowState(views::Widget* widget, HWND hwnd)
      : widget(widget), hwnd(hwnd) {}

  // One step of the fade and of the snap's animation.
  void StepFade();
  void StepBounds();
  // A gesture, a drag or a resize ended: snap.
  void EndGesture() { PipWindows::Get().EndGesture(widget); }

  raw_ptr<views::Widget> widget;
  HWND hwnd;
  bool rounded = false;
  bool scrolls_naturally = true;

  // The hover fade, with SetLayeredWindowAttributes() (the window is layered
  // only while it lets clicks through).
  float opacity = 1.0f;
  float fade_from = 1.0f;
  float fade_to = 1.0f;
  base::TimeTicks fade_start;
  base::RepeatingTimer fade_timer;

  // The snap's animation.
  gfx::Rect bounds_from;
  gfx::Rect bounds_to;
  base::TimeTicks bounds_start;
  base::RepeatingTimer bounds_timer;

  // Ends a pinch or a two-finger move, or (at once, after upstream's
  // handling) a drag or a resize.
  base::OneShotTimer gesture_end_timer;
};

std::map<HWND, std::unique_ptr<WindowState>>& States() {
  static base::NoDestructor<std::map<HWND, std::unique_ptr<WindowState>>>
      states;
  return *states;
}

WindowState* FindState(HWND hwnd) {
  auto it = States().find(hwnd);
  return it == States().end() ? nullptr : it->second.get();
}

void WindowState::StepFade() {
  const double t = std::clamp(
      (base::TimeTicks::Now() - fade_start) / kFadeDuration, 0.0, 1.0);
  opacity = fade_from + (fade_to - fade_from) * static_cast<float>(t);
  if (IsLayered(hwnd)) {
    ::SetLayeredWindowAttributes(hwnd, 0, ToAlpha(opacity), LWA_ALPHA);
  }
  if (t >= 1.0) {
    fade_timer.Stop();
  }
}

void WindowState::StepBounds() {
  const double t = std::clamp(
      (base::TimeTicks::Now() - bounds_start) / kBoundsDuration, 0.0, 1.0);
  if (t >= 1.0) {
    bounds_timer.Stop();
  }
  widget->SetBounds(gfx::Tween::RectValueBetween(
      gfx::Tween::CalculateValue(gfx::Tween::EASE_OUT, t), bounds_from,
      bounds_to));
}

// Takes pinches and two-finger moves aimed at a picture-in-picture window,
// and sees drags and resizes end. Windows sends a precision touchpad's
// pinch as Ctrl+wheel and its two-finger scrolls as wheel messages in
// steps finer than a notch; a mouse wheel's go on as before.
class GestureFilter : public ui::HWNDMessageFilter {
 public:
  bool FilterMessage(HWND hwnd,
                     UINT message,
                     WPARAM w_param,
                     LPARAM l_param,
                     LRESULT* l_result) override {
    WindowState* state = FindState(hwnd);
    if (!state) {
      return false;
    }
    PipWindows& pip_windows = PipWindows::Get();
    if (pip_windows.IsClickThrough(state->widget)) {
      return false;
    }
    switch (message) {
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(w_param);
        const bool ctrl = GET_KEYSTATE_WPARAM(w_param) & MK_CONTROL;
        if (message == WM_MOUSEWHEEL && ctrl) {
          pip_windows.ZoomWindow(state->widget, delta / kWheelUnitsPerZoom);
        } else if (!ctrl && delta % WHEEL_DELTA != 0) {
          // With the fingers, whichever way scrolling goes. A positive
          // vertical delta scrolls up, a positive horizontal one right.
          const double sign = state->scrolls_naturally ? 1 : -1;
          const int move = static_cast<int>(
              std::lround(sign * delta * kDipPerWheelUnit));
          pip_windows.MoveWindow(state->widget,
                                 message == WM_MOUSEWHEEL
                                     ? gfx::Vector2d(0, move)
                                     : gfx::Vector2d(-move, 0));
        } else {
          return false;
        }
        // The touchpad's inertia keeps sending wheel messages after the
        // fingers lift; the window snaps once they stop.
        state->gesture_end_timer.Start(FROM_HERE, kGestureEndDelay, state,
                                       &WindowState::EndGesture);
        *l_result = 0;
        return true;
      }
      case WM_EXITSIZEMOVE:
        // A drag (the window is all caption) or a resize ended. Snap after
        // upstream's own handling of it.
        state->gesture_end_timer.Start(FROM_HERE, base::TimeDelta(), state,
                                       &WindowState::EndGesture);
        return false;
      default:
        return false;
    }
  }
};

GestureFilter& GetGestureFilter() {
  static base::NoDestructor<GestureFilter> filter;
  return *filter;
}

}  // namespace

void PipWindows::PlatformAddWindow(views::Widget* widget) {
  HWND hwnd = GetHwnd(widget);
  if (!hwnd) {
    return;
  }
  auto& slot = States()[hwnd];
  if (!slot) {
    slot = std::make_unique<WindowState>(widget, hwnd);
    ui::HWNDSubclass::AddFilterToTarget(hwnd, &GetGestureFilter());
  }
  slot->scrolls_naturally = TouchpadScrollsNaturally();
  // Rounded corners: the system's, on Windows 11.
  if (base::win::GetVersion() >= base::win::Version::WIN11) {
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    slot->rounded = SUCCEEDED(::DwmSetWindowAttribute(
        hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner)));
  }
}

void PipWindows::PlatformRemoveWindow(views::Widget* widget) {
  HWND hwnd = GetHwnd(widget);
  if (!hwnd) {
    return;
  }
  PlatformSetIgnoresMouse(widget, false);
  if (ui::HWNDSubclass* subclass =
          ui::HWNDSubclass::GetHwndSubclassForTarget(hwnd)) {
    subclass->RemoveFilter(&GetGestureFilter());
  }
  States().erase(hwnd);
}

void PipWindows::PlatformSetIgnoresMouse(views::Widget* widget,
                                         bool ignores) {
  HWND hwnd = GetHwnd(widget);
  if (!hwnd) {
    return;
  }
  // WS_EX_TRANSPARENT lets clicks through a top-level window only when it's
  // layered too.
  constexpr LONG kStyles = WS_EX_LAYERED | WS_EX_TRANSPARENT;
  const LONG style = ::GetWindowLong(hwnd, GWL_EXSTYLE);
  const LONG wanted = ignores ? style | kStyles : style & ~kStyles;
  WindowState* state = FindState(hwnd);
  if (!ignores && state) {
    state->fade_timer.Stop();
    state->opacity = 1.0f;
  }
  if (wanted == style) {
    return;
  }
  ::SetWindowLong(hwnd, GWL_EXSTYLE, wanted);
  if (ignores) {
    // A layered window shows nothing until it has an alpha.
    ::SetLayeredWindowAttributes(
        hwnd, 0, ToAlpha(state ? state->opacity : 1.0f), LWA_ALPHA);
  }
}

void PipWindows::PlatformFade(views::Widget* widget, float opacity) {
  HWND hwnd = GetHwnd(widget);
  WindowState* state = hwnd ? FindState(hwnd) : nullptr;
  if (!state) {
    return;
  }
  state->fade_from = state->opacity;
  state->fade_to = opacity;
  state->fade_start = base::TimeTicks::Now();
  state->fade_timer.Start(FROM_HERE, kFrameInterval, state,
                          &WindowState::StepFade);
  state->StepFade();
}

void PipWindows::PlatformAnimateBounds(views::Widget* widget,
                                       const gfx::Rect& bounds) {
  HWND hwnd = GetHwnd(widget);
  WindowState* state = hwnd ? FindState(hwnd) : nullptr;
  if (!state) {
    widget->SetBounds(bounds);
    return;
  }
  state->bounds_from = widget->GetWindowBoundsInScreen();
  state->bounds_to = bounds;
  state->bounds_start = base::TimeTicks::Now();
  state->bounds_timer.Start(FROM_HERE, kFrameInterval, state,
                            &WindowState::StepBounds);
}

void PipWindows::PlatformUpdateEventMonitor() {
  // Each window's messages are filtered from PlatformAddWindow() to
  // PlatformRemoveWindow().
}

bool PipWindows::IsIgnoringMouseForTesting(views::Widget* widget) const {
  HWND hwnd = GetHwnd(widget);
  return hwnd && (::GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT);
}

bool PipWindows::HasRoundCornersForTesting(views::Widget* widget) const {
  HWND hwnd = GetHwnd(widget);
  WindowState* state = hwnd ? FindState(hwnd) : nullptr;
  return state && state->rounded;
}

bool PipWindows::AllowsTilingForTesting(views::Widget* widget) const {
  // Windows' Snap is left as it is.
  return true;
}

void PipWindows::WillFocusOpener() {}

}  // namespace compass
