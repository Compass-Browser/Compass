// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_PICTURE_IN_PICTURE_PIP_STYLE_H_
#define COMPASS_BROWSER_UI_VIEWS_PICTURE_IN_PICTURE_PIP_STYLE_H_

#include "base/memory/raw_ptr.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

class VideoOverlayWindowViews;

namespace compass {

// Compass's look for video picture-in-picture windows on macOS and Windows
// (docs/design/picture-in-picture.md, "Controls"):
// - No site name or favicon.
// - Back to tab, with its title, in the top left corner; minimize and close
//   in the top right.
// - Each group of controls (top left, top right, the playback or call
//   buttons in the middle, the timeline row at the bottom) on a frosted
//   pill, which fades with the controls; a light dim over the video instead
//   of upstream's dark gradients.
// - The window's colors are seeded with the color of the space the video's
//   tab belongs to (GetPipTint(), Widget::SetUserColorOverride()), as
//   SidebarTint seeds a browser window's: the timeline and focus rings take
//   it, and the pills a hint of it.
//
// Built in upstream's //chrome/browser/ui/views/overlay target (see
// patches/), whose VideoOverlayWindowViews calls in and lets this class see
// its views (friend).
class PipStyle {
 public:
  // Space between the pills and the window's edges.
  static constexpr int kMargin = 8;
  // Space between a pill's edge and the buttons on it.
  static constexpr int kPillPadding = 2;

  // At the end of VideoOverlayWindowViews::SetUpViews(): hides the site
  // name, swaps the scrims for the dim, adds the pills.
  static void SetUp(VideoOverlayWindowViews* window);

  // Runs Layout() when VideoOverlayWindowViews::OnUpdateControlsBounds()
  // returns (it has more than one way out).
  class ScopedLayout {
   public:
    explicit ScopedLayout(VideoOverlayWindowViews* window) : window_(window) {}
    ScopedLayout(const ScopedLayout&) = delete;
    ScopedLayout& operator=(const ScopedLayout&) = delete;
    ~ScopedLayout() { Layout(window_); }

   private:
    const raw_ptr<VideoOverlayWindowViews> window_;
  };

  // Places the top buttons, moves the bottom row clear of the window's
  // corners and puts each pill under its group. Seeds the window's colors
  // again in case the tab moved to another space.
  static void Layout(VideoOverlayWindowViews* window);

  // The pills' bounds in the window, by group, empty when not shown.
  struct PillBounds {
    gfx::Rect top_leading;
    gfx::Rect top_trailing;
    gfx::Rect center;
    gfx::Rect bottom;
  };
  static PillBounds GetPillBoundsForTesting(VideoOverlayWindowViews* window);

 private:
  // The bottom row, after upstream's layout: [previous][time][timeline]
  // [next][mute][captions] on one line.
  static void LayOutBottomRow(VideoOverlayWindowViews* window,
                              const gfx::Size& size);
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_PICTURE_IN_PICTURE_PIP_STYLE_H_
