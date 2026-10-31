// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_PICTURE_IN_PICTURE_PIP_WINDOWS_MAC_H_
#define COMPASS_BROWSER_UI_VIEWS_PICTURE_IN_PICTURE_PIP_WINDOWS_MAC_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/scoped_multi_source_observation.h"
#include "base/timer/timer.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/accelerators/global_accelerator_listener/global_accelerator_listener.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

namespace compass {

// What Compass adds to video picture-in-picture windows on macOS
// (docs/design/picture-in-picture.md):
// - Click-through: Ctrl+Option+Cmd+P, from any app while a window is open,
//   makes it ignore the mouse, so clicks land on what's under it; again makes
//   it take clicks again. Meanwhile it turns translucent under the pointer
//   and shows no controls.
// - Rounded corners.
// - Trackpad gestures: pinch resizes it, two fingers move it, and at the end
//   of a gesture or a drag it snaps to a screen edge it's close to.
class PipWindows : public views::WidgetObserver,
                        public ui::GlobalAcceleratorListener::Observer {
 public:
  // The shortcut, global while a picture-in-picture window is open.
  static ui::Accelerator GetAccelerator();
  // The window's opacity under the pointer while it ignores the mouse.
  static constexpr float kHoverOpacity = 0.25f;
  // Its corners, as the page card's (docs/design/window-frame.md).
  static constexpr double kCornerRadius = 12;
  // A window that ends a move this close to an edge of the screen's work
  // area snaps to it, kEdgeMargin from it.
  static constexpr int kSnapDistance = 32;
  static constexpr int kEdgeMargin = 12;

  // |bounds| scaled by 1 + |magnification| around its center, keeping its
  // aspect ratio, within |min_size| and |max_size|.
  static gfx::Rect Zoom(const gfx::Rect& bounds,
                        double magnification,
                        const gfx::Size& min_size,
                        const gfx::Size& max_size);
  // |bounds| moved by |delta|, kept inside |work_area|.
  static gfx::Rect Move(const gfx::Rect& bounds,
                        const gfx::Vector2d& delta,
                        const gfx::Rect& work_area);
  // |bounds| snapped to the edges of |work_area| it's within kSnapDistance
  // of (kEdgeMargin from them), and kept inside it.
  static gfx::Rect Snap(const gfx::Rect& bounds, const gfx::Rect& work_area);

  static PipWindows& Get();

  PipWindows(const PipWindows&) = delete;
  PipWindows& operator=(const PipWindows&) = delete;

  // A picture-in-picture window showed (VideoOverlayWindowViews::
  // ShowInactive(), see patches/). Tracked until it hides (upstream hides a
  // closed window and may show it again) or is destroyed.
  void AddWindow(views::Widget* widget);

  // Turns click-through on or off for every picture-in-picture window.
  void Toggle();
  bool enabled() const { return enabled_; }
  // Whether |widget| lets clicks through now. Its controls then stay hidden
  // (VideoOverlayWindowViews::UpdateControlsVisibility(), see patches/): the
  // window still sees the pointer move, though not clicks.
  bool IsClickThrough(const views::Widget* widget) const;

  bool IsIgnoringMouseForTesting(views::Widget* widget) const;
  bool is_listening_for_testing() const { return listening_; }
  bool IsHoveredForTesting(views::Widget* widget) const;
  double GetCornerRadiusForTesting(views::Widget* widget) const;
  const std::vector<raw_ptr<views::Widget>>& windows_for_testing() const {
    return windows_;
  }
  // Runs one hover check now.
  void UpdateHoverForTesting() { UpdateHover(); }

  // A pinch on |widget| by |magnification| (NSEvent's), and a move by two
  // fingers. EndGesture() snaps it to an edge it's close to (animated unless
  // |animate| is false).
  void ZoomWindow(views::Widget* widget, double magnification);
  void MoveWindow(views::Widget* widget, const gfx::Vector2d& delta);
  void EndGesture(views::Widget* widget, bool animate = true);

  // views::WidgetObserver:
  void OnWidgetDestroying(views::Widget* widget) override;
  void OnWidgetVisibilityChanged(views::Widget* widget, bool visible) override;

  // ui::GlobalAcceleratorListener::Observer:
  void OnKeyPressed(const ui::Accelerator& accelerator) override;
  void ExecuteCommand(const std::string& accelerator_group_id,
                      const std::string& command_id) override {}

 private:
  friend class base::NoDestructor<PipWindows>;
  PipWindows();
  ~PipWindows() override;

  void Apply(views::Widget* widget);
  // Rounds the window's corners; its shadow follows.
  void RoundCorners(views::Widget* widget);
  // Stops tracking |widget|, which takes clicks again.
  void RemoveWindow(views::Widget* widget);
  // Follows the pointer while click-through is on: the window under it fades.
  void UpdateHover();
  void SetHovered(views::Widget* widget, bool hovered);
  void UpdateListening();
  // Gestures on the windows: an application event monitor (in the .mm)
  // while any is open.
  void UpdateEventMonitor();
  gfx::Rect GetWorkArea(views::Widget* widget) const;

  std::vector<raw_ptr<views::Widget>> windows_;
  // Windows faded under the pointer.
  std::vector<raw_ptr<views::Widget>> hovered_;
  bool enabled_ = false;
  bool listening_ = false;
  base::RepeatingTimer hover_timer_;

  base::ScopedMultiSourceObservation<views::Widget, views::WidgetObserver>
      observations_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_PICTURE_IN_PICTURE_PIP_WINDOWS_MAC_H_
