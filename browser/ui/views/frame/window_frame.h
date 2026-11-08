// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_FRAME_WINDOW_FRAME_H_
#define COMPASS_BROWSER_UI_VIEWS_FRAME_WINDOW_FRAME_H_

#include <optional>

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"

class BrowserWindowInterface;

namespace gfx {
class Point;
}  // namespace gfx

namespace views {
class Widget;
}  // namespace views

namespace compass {

// The frame of the space's color around the page's card, as in Arc
// (content_frame.h sets its measurements; upstream's browser view layout
// insets the page). It's tinted like the sidebar, so the two are one color,
// and while the sidebar is collapsed to the frame's edge the caption buttons
// (macOS's traffic lights) are hidden; they fade with the sidebar as it
// opens and closes, also when it shows on hover. See
// docs/design/window-frame.md.
class WindowFrame : public views::ViewObserver {
 public:
  // Gives |browser|'s window its frame, once. |region| is the vertical tab
  // strip region, which owns it.
  static void Install(views::View* region, BrowserWindowInterface* browser);

  // The installed frame of |region|'s window, if any.
  static WindowFrame* FromRegion(views::View* region);

  WindowFrame(views::View* region, BrowserWindowInterface* browser);
  WindowFrame(const WindowFrame&) = delete;
  WindowFrame& operator=(const WindowFrame&) = delete;
  ~WindowFrame() override;

  bool AreCaptionButtonsHiddenForTesting() const {
    return caption_buttons_hidden_;
  }

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewVisibilityChanged(views::View* observed_view,
                               views::View* starting_view,
                               bool visible) override;
  void OnViewIsDeleting(views::View* observed_view) override;

 private:
  // Fades the caption buttons with the sidebar's width, hidden while it's
  // only the frame's edge. Not in fullscreen, where they're how to leave it.
  void UpdateCaptionButtons();

  raw_ptr<views::View> region_;
  const raw_ptr<BrowserWindowInterface> browser_;
  bool caption_buttons_hidden_ = false;
  float caption_buttons_opacity_ = 1.f;
  // Windows: the widget whose caption buttons were made to ask for their
  // size again (a profile switch brings the view to another widget). Only
  // compared, never followed: that widget may be gone.
  raw_ptr<views::Widget, DisableDanglingPtrDetection>
      sized_caption_buttons_for_ = nullptr;
  base::ScopedObservation<views::View, views::ViewObserver> region_observation_{
      this};
};

// The line the caption buttons (macOS's traffic lights) and the sidebar
// header's top row are centered on, in the window. Upstream (macOS 26 and
// later) puts the 14 DIP lights 13 DIP from the window's leading edge to be
// concentric with its corner; 13 DIP from the top too centers them here.
inline constexpr int kCaptionButtonsCenterY = 13 + 7;

// The title bar height macOS centers the caption buttons of |browser_view|'s
// window on (BrowserNativeWidgetMac::GetWindowFrameTitlebarHeight(), see
// patches/), if the window has the frame and isn't fullscreen. Upstream's
// height comes from the toolbar, which isn't shown.
std::optional<int> GetWindowTitlebarHeight(views::View* browser_view);

// Windows: the caption buttons' size (minimize, maximize, close), each, in
// |browser_view|'s window if it has the frame: smaller than upstream's 45x29
// DIP, which is a title bar's height, so the strip they leave above the page
// is thin (WindowsCaptionButton::CalculatePreferredSize(), patch). Null
// without the frame.
inline constexpr gfx::Size kCompactCaptionButtonSize(36, 24);
std::optional<gfx::Size> GetCompactCaptionButtonSize(
    views::View* browser_view);

// Whether |point| (in |browser_view|'s coordinates) is on the frame around
// the page, where nothing else is: it drags the window, as a title bar does.
bool IsOnWindowFrame(views::View* browser_view, const gfx::Point& point);

// Sets |widget|'s caption buttons' opacity, hidden at 0. Only macOS has any.
void SetCaptionButtonsOpacity(views::Widget* widget, float opacity);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_FRAME_WINDOW_FRAME_H_
