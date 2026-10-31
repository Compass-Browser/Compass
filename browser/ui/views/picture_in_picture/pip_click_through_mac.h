// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_PICTURE_IN_PICTURE_PIP_CLICK_THROUGH_MAC_H_
#define COMPASS_BROWSER_UI_VIEWS_PICTURE_IN_PICTURE_PIP_CLICK_THROUGH_MAC_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/scoped_multi_source_observation.h"
#include "base/timer/timer.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/accelerators/global_accelerator_listener/global_accelerator_listener.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

namespace compass {

// Click-through picture-in-picture (docs/design/picture-in-picture.md): while
// a video's picture-in-picture window is open, Ctrl+Option+Cmd+P, from any
// app, makes it ignore the mouse, so clicks land on what's under it; again
// makes it take clicks again. While it ignores the mouse it turns
// translucent under the pointer, to show what's under it.
class PipClickThrough : public views::WidgetObserver,
                        public ui::GlobalAcceleratorListener::Observer {
 public:
  // The shortcut, global while a picture-in-picture window is open.
  static ui::Accelerator GetAccelerator();
  // The window's opacity under the pointer while it ignores the mouse.
  static constexpr float kHoverOpacity = 0.25f;
  // Its corners, as the page card's (docs/design/window-frame.md).
  static constexpr double kCornerRadius = 12;

  static PipClickThrough& Get();

  PipClickThrough(const PipClickThrough&) = delete;
  PipClickThrough& operator=(const PipClickThrough&) = delete;

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

  // views::WidgetObserver:
  void OnWidgetDestroying(views::Widget* widget) override;
  void OnWidgetVisibilityChanged(views::Widget* widget, bool visible) override;

  // ui::GlobalAcceleratorListener::Observer:
  void OnKeyPressed(const ui::Accelerator& accelerator) override;
  void ExecuteCommand(const std::string& accelerator_group_id,
                      const std::string& command_id) override {}

 private:
  friend class base::NoDestructor<PipClickThrough>;
  PipClickThrough();
  ~PipClickThrough() override;

  void Apply(views::Widget* widget);
  // Rounds the window's corners; its shadow follows.
  void RoundCorners(views::Widget* widget);
  // Stops tracking |widget|, which takes clicks again.
  void RemoveWindow(views::Widget* widget);
  // Follows the pointer while click-through is on: the window under it fades.
  void UpdateHover();
  void SetHovered(views::Widget* widget, bool hovered);
  void UpdateListening();

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

#endif  // COMPASS_BROWSER_UI_VIEWS_PICTURE_IN_PICTURE_PIP_CLICK_THROUGH_MAC_H_
