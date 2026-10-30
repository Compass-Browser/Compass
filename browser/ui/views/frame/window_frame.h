// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_FRAME_WINDOW_FRAME_H_
#define STRATA_BROWSER_UI_VIEWS_FRAME_WINDOW_FRAME_H_

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"

class BrowserWindowInterface;

namespace gfx {
class Point;
}  // namespace gfx

namespace views {
class Widget;
}  // namespace views

namespace strata {

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
  base::ScopedObservation<views::View, views::ViewObserver> region_observation_{
      this};
};

// Whether |point| (in |browser_view|'s coordinates) is on the frame around
// the page, where nothing else is: it drags the window, as a title bar does.
bool IsOnWindowFrame(views::View* browser_view, const gfx::Point& point);

// Sets |widget|'s caption buttons' opacity, hidden at 0. Only macOS has any.
void SetCaptionButtonsOpacity(views::Widget* widget, float opacity);

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_FRAME_WINDOW_FRAME_H_
